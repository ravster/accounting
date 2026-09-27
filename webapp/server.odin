package main

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

main :: proc() {
	if (len(os.args) != 3) {
		fmt.eprintln("Are you sure about that?\nUsage: PROG 3004 ../data/")
		os.exit(1)
	}
	fmt.printfln("Got %d args", len(os.args))
	fmt.printfln("- %s\n- %s\n- %s", os.args[0], os.args[1], os.args[2])
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

}
