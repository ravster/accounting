package main

import "core:thread"
import "core:time"
import "core:net"
import "core:strconv"
import "core:fmt"
import "core:os"
import "core:path/filepath"
import "core:strings"

Acc :: struct {
	id: u16,
	name: string,
	type: u8
}
Accs: [dynamic]Acc

Tx :: struct {
	id: u16,
	amount: f32,
	note: string,
	debit_account_id: u16,
	credit_account_id: u16,
	created_at: u32
}
Txs: [dynamic]Tx

// read from file
// data := "Data payload text.\n"
// _, write_err := os.write(handle, transmute([]byte)data)
// if write_err != os.ERROR_NONE {
//     fmt.eprintfln("Write error: %v", write_err)
// }

handle_client_socket :: proc(sock: net.TCP_Socket) {
	defer net.close(sock)
	buf: [2048]byte
	for {
		fmt.println("inside handle_client_socket loop")
		timeout := 2* time.Minute
		net.set_option(sock, .Receive_Timeout, timeout)
		net.set_option(sock, .Send_Timeout, timeout)

		bytes_read, read_err := net.recv_tcp(sock, buf[:])
		if read_err != nil {
			#partial switch read_err {
			case .Would_Block: // Timeout
				fmt.println("Timeout. Closing client-socket.")
			case .Connection_Closed:
				fmt.println("Connection closed on client-socket.")
			case:
				fmt.printfln("Error reading from TCP: %v", read_err)
			}
			break
		}
		if bytes_read == 0 {
			// Browsers sometimes preemptively make TCP conns for keep-alive or whatever.
			fmt.println("No bytes read from TCP")
			break
		}
		fmt.printfln("Received this from TCP:%s", string(buf[:bytes_read]))
		// parse_get params
		// parse_body post params. Requires reading whole http body.
		// parse route
		// hand to endpoint handler inside a switch. Pass in only the params type that is needed.
		// output should be a string with the full http response
		response := "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nContent-Type: text/plain\r\n\r\nHello World!"
		net.send_tcp(sock, transmute([]byte)response)
	}
	fmt.println("Ending handle_client_socket")
}

main :: proc() {
	if (len(os.args) != 3) {
		fmt.eprintln("Are you sure about that?\nUsage: PROG 3004 ../data/")
		os.exit(1)
	}
	fmt.printfln("Got %d args", len(os.args))
	fmt.printfln("- %s\n- %s\n- %s", os.args[0], os.args[1], os.args[2])
	port, _ := strconv.parse_int(os.args[1])
	dirpath := os.args[2]

	// File open and read accounts
	accFileName := "accounts.tsv"
	accpath, _ := filepath.join({ dirpath, accFileName })
	defer delete(accpath)
	accFile, err := os.open(accpath, os.O_RDWR)
	if err != os.ERROR_NONE {
		fmt.eprintln("Couldn't open accpath %s", accpath)
		os.exit(1)
	}
	defer os.close(accFile)
	accdata, read1err := os.read_entire_file_from_path(accpath, context.allocator)
	if read1err != os.ERROR_NONE {
		fmt.eprintln("Can't read file:", accpath)
		os.exit(1)
	}
	defer delete(accdata)
	filestr := string(accdata)
	first := true
	for line in strings.split_lines_iterator(&filestr) {
		if first {
			fmt.printfln("Read %s with headers: %s", accpath, line)
			first = false
			continue
		}
		fmt.printfln("%s", line)
		items, _ := strings.split(line, "\t")
		accint, _ := strconv.parse_int(items[0])
		acc16 := u16(accint)
		typeint, _ := strconv.parse_int(items[2])
		type8 := u8(typeint)
		newtx := Acc{acc16, items[1], type8}
		append(&Accs, newtx)
	}

	// File open and read transactions
	accFileName = "transactions.tsv"
	accpath, _ = filepath.join({ dirpath, accFileName })
	accFile, err = os.open(accpath, os.O_RDWR)
	if err != os.ERROR_NONE {
		fmt.eprintln("Couldn't open txpath %s", accpath)
		os.exit(1)
	}
	accdata, read1err = os.read_entire_file_from_path(accpath, context.allocator)
	if read1err != os.ERROR_NONE {
		fmt.eprintln("Can't read file:", accpath)
		os.exit(1)
	}
	filestr = string(accdata)
	first = true
	for line in strings.split_lines_iterator(&filestr) {
		if first {
			fmt.printfln("Read %s with headers: %s", accpath, line)
			first = false
			continue
		}
		fmt.printfln("%s", line)
		items, _ := strings.split(line, "\t")
		txi, _ := strconv.parse_int(items[0])
		tx16 := u16(txi)
		amount, _ := strconv.parse_f32(items[1])
		note := items[2]
		debiti, _ := strconv.parse_int(items[3])
		debit16 := u16(debiti)
		crediti, _ := strconv.parse_int(items[3])
		credit16 := u16(crediti)
		createdati, _ := strconv.parse_int(items[3])
		createdat32 := u32(createdati)
		newtx := Tx{tx16, amount, note, debit16, credit16, createdat32}
		append(&Txs, newtx)
	}

	// Listen on port
	endpoint := net.Endpoint {
		address = net.IP4_Address{127, 0,0,1},
		port = port
	}
	server_socket, tcpListenErr := net.listen_tcp(endpoint)
	if tcpListenErr != nil {
		fmt.printfln("Can't listen on port:%d. Got err:%v", port, tcpListenErr)
		os.exit(1)
	}
	defer net.close(server_socket)
	fmt.printfln("Listening on port:%d", port)

	// Hand over to new thread inside a listening loop
	for {
		client_socket, client_endpoint, accept_err := net.accept_tcp(server_socket)
		if accept_err != nil {
			fmt.printfln("Failed to accept connection: %v", accept_err)
			continue
		}
		sock2 := new(net.TCP_Socket)
		sock2^ = client_socket
		thread := thread.create_and_start_with_data(sock2, proc(data: rawptr) {
			fmt.println("Start thread")
			sock3 := (^net.TCP_Socket)(data)
			defer free(sock3)
			defer net.close(sock3^)
			handle_client_socket(sock3^)
			fmt.println("Stop thread")
		}, self_cleanup = true)
	}
}
