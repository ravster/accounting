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
	ID            int64
	Amount        float64
	Note          string
	DebitAccountId  int64
	CreditAccountId int64
	CreatedAt     time.Time
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
	tmpl.Execute(w, entries)
}

type Account struct {
	ID int64
	Name string
	Type int
	// enum: Income, Expense, Asset, Liability
}
var accountTypeNames = map[int]string {
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

func createTransaction(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		msg := fmt.Sprintf("Couldn't parse form: %s", err)
		log.Println(msg)
		http.Error(w, msg, 422)
		return
	}

	debitAccount := r.FormValue("debit_account")
	creditAccount := r.FormValue("credit_account")
	note := r.FormValue("note")
	amount_s := r.FormValue("amount")

	if debitAccount == creditAccount {
		msg := fmt.Sprintf("debit (%s) and credit (%s) accounts MUST be different", debitAccount, creditAccount)
		log.Println(msg)
		http.Error(w, msg, 422)
		return
	}

	amount, err := strconv.ParseFloat(amount_s, 64)
	if err != nil {
		msg := fmt.Sprintf("Couldn't parse float: amount (%s): %e", amount_s, err)
		log.Println(msg)
		http.Error(w, msg, 422)
		return
	}

	_, err = db.Exec(`
		INSERT into transactions (amount, note, debit_account, credit_account)
		VALUES ($1, $2, $3, $4)`,
		amount, note, debitAccount, creditAccount,
	)
	if err != nil {
		msg := fmt.Sprintf("Couldn't write to DB: %s", err)
		log.Println(msg)
		http.Error(w, msg, 422)
		return
	}

	http.Redirect(w, r, "/ledger", 303)
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
	// TODO
	// Tag account as income (customers)
	// Tag as expense (rent, Sin, NR, CamCK, EDC, water, wifi, depreciation, etc)
	// Tag as asset (aba , cash, working-capital)

	log.Println("Server starting on http://localhost:3002")
	if err := http.ListenAndServe(":3002", nil); err != nil {
		log.Fatal(err)
	}
}
