package main

import (
	"database/sql"
	"fmt"
	"os"

	_ "github.com/lib/pq" // Pure Go Postgres driver registered anonymously
)

func main() {
	fmt.Println("Hello world!")

	// The connection string is completely identical to your Odin implementation
	connStr := "host=localhost password=password port=5432 user=postgres dbname=accounting sslmode=disable"

	// Open establishes the pool structure; it does not connect to the network instantly
	db, err := sql.Open("postgres", connStr)
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
}

