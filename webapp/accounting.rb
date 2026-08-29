require 'socket'
require "debug"

$accs = []
$txs = []

Acc = Struct.new(:id, :name, :type)
Tx = Struct.new(:id, :amount, :note, :debit, :credit, :created)

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
