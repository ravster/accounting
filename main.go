package main

import (
	"database/sql"
	"fmt"
	"html/template"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"time"

	_ "github.com/lib/pq" // Pure Go Postgres driver registered anonymously
)

var db *sql.DB
var account_names = make(map[int64]string)

// Load this in-memory so we don't have to query the DB for this mapping repeatedly, and we
// don't need the DB to do the join repeatedly. This will be run once, during program startup,
// so if you add new accounts, restart the program. Adding new accounts is a rare operation, so
// forcing a program restart is acceptable.
func loadAccountNamesCache() error {
	rows, err := db.Query(`select id, name from accounts`)
	if err != nil {
		msg := fmt.Sprintf("DB err: %s", err)
		log.Println(msg)
		return err
	}
	defer rows.Close()

	for rows.Next() {
		var id int64
		var name string
		err := rows.Scan(
			&id,
			&name)
		if err != nil {
			msg := fmt.Sprintf("Row scan err: %s", err)
			log.Println(msg)
			return err
		}
		account_names[id] = name
	}
	return nil
}

type Transaction struct {
	ID              int64
	Amount          float64
	Note            string
	DebitAccountId  int64
	CreditAccountId int64
	CreatedAt       time.Time
}

func homeHandler(w http.ResponseWriter, r *http.Request) {
	log.Println("Serving home")
	tmplPath := filepath.Join("templates", "home.html")

	// Parse the template file on every request (helpful for development changes)
	tmpl, err := template.ParseFiles(tmplPath)
	if err != nil {
		log.Printf("Template parsing error: %v", err)
		http.Error(w, "Internal Server Error", http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")

	// Render the template directly to the HTTP response writer
	err = tmpl.Execute(w, nil)
	if err != nil {
		log.Printf("Template execution error: %v", err)
	}
}

func getLedger(w http.ResponseWriter, r *http.Request) {
	rows, err := db.Query(`
		SELECT id, amount, note, debit_account_id, credit_account_id, created_at
		FROM transactions
		ORDER BY created_at DESC
		LIMIT 30`)
	if err != nil {
		msg := fmt.Sprintf("DB err: %s", err)
		log.Println(msg)
		http.Error(w, msg, 500)
		return
	}
	defer rows.Close()

	var entries []Transaction
	for rows.Next() {
		var e Transaction
		err := rows.Scan(
			&e.ID,
			&e.Amount,
			&e.Note,
			&e.DebitAccountId,
			&e.CreditAccountId,
			&e.CreatedAt,
		)
		if err != nil {
			msg := fmt.Sprintf("Row scan err: %s", err)
			log.Println(msg)
			http.Error(w, msg, 500)
			return
		}
		entries = append(entries, e)
	}

	tmplPath := filepath.Join("templates", "ledger.html")
	tmpl, err := template.ParseFiles(tmplPath)
	if err != nil {
		msg := fmt.Sprintf("Template parsing err: %s", err)
		log.Println(msg)
		http.Error(w, msg, 500)
		return
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	data := map[string]interface{}{
		"entries":  entries,
		"accounts": account_names,
	}
	tmpl.Execute(w, data)
}

func createTransaction(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		msg := fmt.Sprintf("Couldn't parse form: %s", err)
		log.Println(msg)
		http.Error(w, msg, 422)
		return
	}

	debit_id_s := r.FormValue("debit_account_id")
	debit_id, err := strconv.ParseInt(debit_id_s, 10, 64)
	if err != nil {
		msg := fmt.Sprintf("Can't parse debit_account_id=%s, err=%s", debit_id_s, err)
		log.Println(msg)
		http.Error(w, msg, 422)
		return
	}
	credit_id_s := r.FormValue("credit_account_id")
	credit_id, err := strconv.ParseInt(credit_id_s, 10, 64)
	if err != nil {
		msg := fmt.Sprintf("Can't parse credit_account_id=%s, err=%s", credit_id_s, err)
		log.Println(msg)
		http.Error(w, msg, 422)
		return
	}
	if debit_id == credit_id {
		msg := fmt.Sprintf("debit_id (%d) and credit_id (%d) accounts MUST be different", debit_id, credit_id)
		log.Println(msg)
		http.Error(w, msg, 422)
		return
	}

	note := r.FormValue("note")
	amount_s := r.FormValue("amount")

	amount, err := strconv.ParseFloat(amount_s, 64)
	if err != nil {
		msg := fmt.Sprintf("Couldn't parse float: amount (%s): %e", amount_s, err)
		log.Println(msg)
		http.Error(w, msg, 422)
		return
	}

	_, err = db.Exec(`
		INSERT into transactions (amount, note, debit_account_id, credit_account_id)
		VALUES ($1, $2, $3, $4)`,
		amount, note, debit_id, credit_id,
	)
	if err != nil {
		msg := fmt.Sprintf("Couldn't write to DB: %s", err)
		log.Println(msg)
		http.Error(w, msg, 422)
		return
	}

	http.Redirect(w, r, "/ledger", 303)
}

type Account struct {
	ID   int64
	Name string
	Type int
	// enum: Income, Expense, Asset, Liability
}

var accountTypeNames = map[int]string{
	0: "Income",
	1: "Expense",
	2: "Asset",
	3: "Liability",
}

func formatAccountType(t int) string {
	name, ok := accountTypeNames[t]
	if ok {
		return name
	}
	return "ERROR UNKNOWN TYPE"
}

func listAccounts(w http.ResponseWriter, r *http.Request) {
	rows, err := db.Query(`
	  select id, name, type from accounts
	`)
	if err != nil {
		msg := fmt.Sprintf("DB err: %s", err)
		log.Println(msg)
		http.Error(w, msg, 500)
		return
	}
	defer rows.Close()

	var accounts []Account
	for rows.Next() {
		var acct Account
		err := rows.Scan(&acct.ID, &acct.Name, &acct.Type)
		if err != nil {
			msg := fmt.Sprintf("Row scan err: %s", err)
			log.Println(msg)
			http.Error(w, msg, 500)
			return
		}
		accounts = append(accounts, acct)
	}

	funcMap := template.FuncMap{
		"resolveType": formatAccountType,
	}

	tmplPath := filepath.Join("templates", "listAccounts.html")
	tmpl, err := template.New(filepath.Base(tmplPath)).Funcs(funcMap).ParseFiles(tmplPath)
	if err != nil {
		msg := fmt.Sprintf("Template parsing err: %s", err)
		log.Println(msg)
		http.Error(w, msg, 500)
		return
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	tmpl.Execute(w, accounts)
}

func createAccount(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		msg := fmt.Sprintf("Couldn't parse form: %s", err)
		log.Println(msg)
		http.Error(w, msg, 422)
		return
	}

	name := r.FormValue("name")
	type_s := r.FormValue("type")

	type_i, err := strconv.ParseFloat(type_s, 64)
	if err != nil {
		msg := fmt.Sprintf("Couldn't parse float: type (%s): %e", type_s, err)
		log.Println(msg)
		http.Error(w, msg, 422)
		return
	}

	_, err = db.Exec(`
		INSERT into accounts (name, type)
		VALUES ($1, $2)`,
		name, type_i,
	)
	if err != nil {
		msg := fmt.Sprintf("Couldn't write to DB: %s", err)
		log.Println(msg)
		http.Error(w, msg, 422)
		return
	}

	http.Redirect(w, r, "/accounts", 303)
}

func incomeStatement(w http.ResponseWriter, r *http.Request) {
	// TODO
	// parse query
	// if query has "-", do month and year
	// else just do year
	// calc start
	// calc stop
	// find all accounts of type=income for the period, and their transactions, and do debit-credit.
	// find total of all expense accounts for the period.
	// Display data to user
	rows, err := db.Query(`
	  select id, name, type from accounts
	`)
	if err != nil {
		msg := fmt.Sprintf("DB err: %s", err)
		log.Println(msg)
		http.Error(w, msg, 500)
		return
	}
	defer rows.Close()

	var accounts []Account
	for rows.Next() {
		var acct Account
		err := rows.Scan(&acct.ID, &acct.Name, &acct.Type)
		if err != nil {
			msg := fmt.Sprintf("Row scan err: %s", err)
			log.Println(msg)
			http.Error(w, msg, 500)
			return
		}
		accounts = append(accounts, acct)
	}

	funcMap := template.FuncMap{
		"resolveType": formatAccountType,
	}

	tmplPath := filepath.Join("templates", "listAccounts.html")
	tmpl, err := template.New(filepath.Base(tmplPath)).Funcs(funcMap).ParseFiles(tmplPath)
	if err != nil {
		msg := fmt.Sprintf("Template parsing err: %s", err)
		log.Println(msg)
		http.Error(w, msg, 500)
		return
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	tmpl.Execute(w, accounts)
}

func main() {
	fmt.Println("Hello world!")
	var err error

	// The connection string is completely identical to your Odin implementation
	connStr := "host=localhost password=password port=5432 user=postgres dbname=accounting sslmode=disable"

	// Open establishes the pool structure; it does not connect to the network instantly
	db, err = sql.Open("postgres", connStr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "DB configuration failed: %v\n", err)
		return
	}
	defer db.Close()

	// Ping forces a network roundtrip to explicitly check if the server is reachable
	err = db.Ping()
	if err != nil {
		fmt.Fprintf(os.Stderr, "DB connection failed: %v\n", err)
		return
	}
	fmt.Println("DB connection passed.")
	loadAccountNamesCache()

	http.HandleFunc("GET /", homeHandler)
	http.HandleFunc("GET /ledger", getLedger)
	http.HandleFunc("POST /transactions", createTransaction)
	http.HandleFunc("GET /accounts", listAccounts)
	http.HandleFunc("POST /accounts", createAccount)
	http.HandleFunc("GET /income_statement", incomeStatement)

	log.Println("Server starting on http://localhost:3002")
	if err := http.ListenAndServe(":3002", nil); err != nil {
		log.Fatal(err)
	}
}
