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

type Transaction struct {
	ID            int64
	Amount        float64
	Note          string
	DebitAccount  string
	CreditAccount string
	CreatedAt     time.Time
}

var db *sql.DB

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
		SELECT id, amount, note, debit_account, credit_account, created_at
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
			&e.DebitAccount,
			&e.CreditAccount,
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

func listAccounts(w http.ResponseWriter, r *http.Request) {
	rows, err := db.Query(`
		select debit_account as account_name from transactions
		union
		select credit_account as account_name from transactions
		order by account_name asc
		`)
	if err != nil {
		msg := fmt.Sprintf("DB err: %s", err)
		log.Println(msg)
		http.Error(w, msg, 500)
		return
	}
	defer rows.Close()

	var account_names []string
	for rows.Next() {
		var name string
		err := rows.Scan(&name)
		if err != nil {
			msg := fmt.Sprintf("Row scan err: %s", err)
			log.Println(msg)
			http.Error(w, msg, 500)
			return
		}
		account_names = append(account_names, name)
	}

	tmplPath := filepath.Join("templates", "listAccounts.html")
	tmpl, err := template.ParseFiles(tmplPath)
	if err != nil {
		msg := fmt.Sprintf("Template parsing err: %s", err)
		log.Println(msg)
		http.Error(w, msg, 500)
		return
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	tmpl.Execute(w, account_names)
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

	http.HandleFunc("GET /", homeHandler)
	http.HandleFunc("GET /ledger", getLedger)
	http.HandleFunc("POST /transactions", createTransaction)
	http.HandleFunc("GET /accounts", listAccounts)
	// TODO
	// Tag account as income (customers)
	// Tag as expense (rent, Sin, NR, CamCK, EDC, water, wifi, depreciation, etc)
	// Tag as asset (aba , cash, working-capital)

	log.Println("Server starting on http://localhost:3002")
	if err := http.ListenAndServe(":3002", nil); err != nil {
		log.Fatal(err)
	}
}
