require 'socket'
require "debug"

$accs = []
$txs = []

Acc = Struct.new(:id, :name, :type)
Tx = Struct.new(:id, :amount, :note, :debit, :credit, :created)
BsAcc = Struct.new(:id, :name, :total)

def load_filedata(acc_path, tx_path)
	File.foreach(acc_path) do |line|
		match = line.match(/(\d+)\t([^\t]+)\t(\d+)/)
		unless match
			next
		end
		id, name, type = match.captures
		id = id.to_i
		type = type.to_i
		$accs << Acc.new(id, name, type)
	end
	File.foreach(tx_path) do |line|
		match = line.match(/(\d+)\t([\d.]+)\t([^\t]+)\t(\d+)\t(\d+)\t(\d+)/)
		unless match
			next
		end
		id, amount, note, debit, credit, created = match.captures
		id = id.to_i
		amount = amount.to_f
		debit = debit.to_i
		credit = credit.to_i
		created = created.to_i
		$txs << Tx.new(id, amount, note, debit, credit, created)
	end
	nil
end

def acc_name(id)
	$accs.find {|acc| acc.id == id }.name
end

def ledger_newest_30_newstr
	out = ""
	$txs.each { |tx|
		debit_name = acc_name(tx.debit)
		credit_name = acc_name(tx.credit)
		p debit_name, credit_name, "z1"
		tr = "<tr> <td>#{tx.id}</td> <td>#{tx.created}</td> <td>#{debit_name}</td> <td>#{credit_name}</td> <td>#{tx.note}</td> <td>#{tx.amount}</td> </tr>"
		out << tr
	}
	out
end

def account_selection_options_new
	out = ""
	$accs.each { |acc|
		out << "<option value=\"#{acc.id}\">#{acc.name}</option>"
	}
	out
end

def write_to_client(client_socket, status, body)
	full_response = sprintf("HTTP/1.1 %d \r\nContent-Length: %d\r\n\r\n%s",
		status, body.size, body)
	client_socket.print(full_response)
end

def listLedger(client_socket, first_line)
	template = File.read("templates/ledger.html")
	ln30 = ledger_newest_30_newstr();
	aso = account_selection_options_new();
	body = sprintf(template, aso, aso, ln30)
	write_to_client(client_socket, 200, body);
end

def tr_of_every_account
	acc_types = %w[ Income Expense Asset Liability ]
	out = ""
	$accs.each { |acc|
		type = acc_types[acc.type];
		temp = sprintf(<<~TR, acc.id, acc.name, type)
			<tr>
			  <td>%d</td>
			  <td>%s</td>
			  <td>%s</td>
			</tr>\n
		TR
		out << temp
	}
	out
end

def listAccounts(client_socket)
	body = File.read("templates/listAccounts.html");
	trs = tr_of_every_account();
	a1 = sprintf(body, trs)
	write_to_client(client_socket, 200, a1);
end

def calc_month(getParams)
	month = getParams['m']
	year = getParams['y']
	if (!month || !year)
		t = Time.now
		month = t.month
		year = t.year
	else
		month = month.to_i
		year = year.to_i
	end

	start = (year* 10000) + (month * 100) + 1
	endMonth = month +1
	endYear = year
	if (endMonth == 13)
		endMonth = 1
		endYear += 1
	end
	stop = (endYear * 10000) + (endMonth * 100) + 1

	prevYear = year
	prevMonth = month -1
	if prevMonth == 0
		prevMonth = 12
		prevYear -=1
	end
	prevLink = "m=#{prevMonth}&y=#{prevYear}"

	nextYear = year
	nextMonth = month +1
	if (nextMonth == 13)
		nextMonth = 1
		nextYear +=1
	end
	nextLink = "m=#{nextMonth}&y=#{nextYear}"

	[month, year, start, stop, prevLink, nextLink]
end

def calcGetParams(first_line)
	url = first_line.match(/ \/\d+\?([^\s]+)/)
	return({}) unless url
	captures = url.captures
	return({}) if captures.empty?
	a1 = captures[0]
	out = {}
	a1.split("&").each { |pair|
		k, v = pair.split("=")
		out[k] = v
	}
	out
end

def incomeStatement(client_socket, first_line)
	template = File.read("templates/incomeStatement.html");
	getParams = calcGetParams(first_line)
	month, year, start, stop, prevLink, nextLink = calc_month(getParams)
	periodTxs = $txs.select { |tx| (tx.created >= start) && (tx.created < stop) }
	incomeTrs = ""
	expenseTrs = ""
	netProfitDollars = 0.0
	tot = 0.0

	$accs.each { |acc|
		case acc.type
		when 0 # income
			tot = 0.0
			periodTxs.each { |tx|
				next if (tx.credit != acc.id)
				tot += tx.amount
			}
			netProfitDollars += tot
			tr = sprintf(<<~TR, acc.name, tot)
				<tr> <td>%s</td>
				     <tx>%.2f</td>
					 <td></td>
				</tr>\n
			TR
			incomeTrs << tr
		when 1 # expense
			tot = 0.0
			periodTxs.each { |tx|
				next if (tx.debit != acc.id)
				tot += tx.amount
			}
			netProfitDollars += tot

			tr = sprintf(<<~TR, acc.name, tot)
				<tr> <td>%s</td>
					 <td></td>
					 <td>%.2f</td>
				</tr>\n
			TR
			expenseTrs << tr
		else
			next
		end
	}

	trs = incomeTrs.concat(expenseTrs)

	body = sprintf(template, prevLink, nextLink, trs, netProfitDollars)
	write_to_client(client_socket, 200, body);
end

def bs_accs_populate_new
	assets = []
	liabilities = []
	$accs.each { |acc|
		case acc.type
		when 2 # asset
			assets << BsAcc.new(acc.id, acc.name, 0)
		when 3 # liability
			liabilities << BsAcc.new(acc.id, acc.name, 0)
		else
			next
		end
	}
	[ assets, liabilities ]
end

def bs_accs_calc_totals(bsAssets, bsLiabilities, stop)
	$txs.each { |tx|
		next if tx.created > stop
		bsAcc = bsAssets.find {|it| it.id == tx.debit }
		if bsAcc # The current tx adds to an asset.
			bsAcc.total += tx.amount
		elsif (bsAcc = bsLiabilities.find {|it| it.id == tx.credit })
			# The current tx adds to a liability.
			bsAcc.total += tx.amount
		end
		bsAcc = bsAssets.find { |it| it.id == tx.credit }
		if bsAcc # Current tx reduces asset.
			bsAcc.total -= tx.amount
		elsif (bsAcc = bsLiabilities.find { |it| it.id == tx.debit })
			bsAcc.total -= tx.amount
		end
	}
end

def bs_accs_trs_new(bsAccs, accType)
	out = ""
	bsAccs.each { |it|
		case accType
		when 2
			out << sprintf("<tr><td>%s</td><td>%d</td><td></td></tr>\n", it.name, it.total)
		when 3
			out << sprintf("<tr><td>%s</td><td></td><td>%d</td></tr>\n", it.name, it.total)
		else
			p "bs_accs_trs_new. Shouldn't have gotten to this else-branch. accType=#{accType}"
		end
	}
	out
end

def balanceSheet(client_socket, first_line)
	template = File.read("templates/balanceSheet.html");
	getParams = calcGetParams(first_line)
	month, year, start, stop, prevLink, nextLink = calc_month(getParams)
	assets, liabilities = bs_accs_populate_new
	bs_accs_calc_totals(assets, liabilities, stop);
	trs_a = bs_accs_trs_new(assets, 2);
	trs_l = bs_accs_trs_new(liabilities, 3);
	body = sprintf(template, prevLink, nextLink, trs_a, trs_l)
	write_to_client(client_socket, 200, body);
end


# Called with a fresh client_socket. Should loop through multiple requets over a persistent connection.
# Output full response string.
def route_request(client_socket)
	headers = []
	content_length = 0
	while (line = client_socket.gets)
		line.strip!
		break if line.empty?

		headers << line

		is_length = line.match(/^Content-Length:\s+(\d+)/)
		if is_length
			content_length = is_length.captures[0]
		end
	end

	if headers.empty?
		msg = "No headers."
		out = "HTTP/1.1 422\r\nContent-Type:text/plain\r\nContent-Length:#{msg.size}\r\n\r\n#{msg}"
		return out
	end

	body = ""
	if content_length > 0
		if content_length > 2048
			msg = "Request body to large. Must be <2048 bytes."
			out = "HTTP/1.1 422\r\nContent-Type:text/plain\r\nContent-Length:#{msg.size}\r\n\r\n#{msg}"
			return out
		end
		body = client_socket.read(content_length)
	end

	first_line = headers[0]
	p first_line, "first_line"
	match = first_line.match(/\w+\s+([^\s]+)/)
	endpoint = match.captures[0]
	p endpoint, "endpoint"
	found = endpoint.match(/\/(\d+)/)
	unless found
		msg = "Not found."
		out = "HTTP/1.1 401\r\nContent-Type:text/plain\r\nContent-Length:#{msg.size}\r\n\r\n#{msg}"
		return out
	end
	p found, "found"
	endpoint2 = found.captures[0].to_i
	p endpoint2, "endpoint2"
	case endpoint2
	when 1
		listLedger(client_socket, first_line) # Doesn't need anything but the GET params
	when 2
		listAccounts(client_socket)
	when 5
		incomeStatement(client_socket, first_line)
	when 6
		balanceSheet(client_socket, first_line)
		# router func
		# parse GET
		# parse POST
		# request body limited to 2kb
		# send params and body to func
		# func returns full response
		# write response to client_socket
		# next loop
	else
		msg = "Not found."
		out = "HTTP/1.1 401\r\nContent-Type:text/plain\r\nContent-Length:#{msg.size}\r\n\r\n#{msg}"
		return out
	end
rescue Errno::ECONNRESET
	p "route_request. Connection reset."
end

def handle_client_socket(client_socket)
	loop do
		out = route_request(client_socket)
		client_socket.print(out)
	rescue Errno::EPIPE, Errno::ECONNRESET => e
		p "Connection closed by peer. Not handling this socket anymore. e.msg=#{e.message}"
		break
	end
end

def main(args)
	dir = args[0]
	account_file = File.read("#{dir}accounts.tsv")
	tx_file = File.read("#{dir}transactions.tsv")

	err = load_filedata("#{dir}accounts.tsv", "#{dir}transactions.tsv")
	if err
		exit(1)
	end
	p $accs, $txs

	server = TCPServer.new("localhost", 3003)
	loop do
		Thread.start(server.accept) do |client_socket|
			puts "New thread"
			handle_client_socket(client_socket)
			client_socket.close
			puts "Done thread"
		end
	end
end

main(ARGV)
