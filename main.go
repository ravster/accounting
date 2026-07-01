package main

import (
	"database/sql"
	"fmt"
	"html/template"
	"log"
	"net/http"
	"os"
	"path/filepath"
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

	log.Println("Server starting on http://localhost:3002")
	if err := http.ListenAndServe(":3002", nil); err != nil {
		log.Fatal(err)
	}
}
