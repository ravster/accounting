create database accounting;
\c accounting

CREATE TABLE transactions (
    id BIGSERIAL PRIMARY KEY,
    amount NUMERIC(10, 2) NOT NULL CHECK (amount > 0.00),
    note VARCHAR(256) NOT NULL,
    debit_account VARCHAR(100) NOT NULL,
    credit_account VARCHAR(100) NOT NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP NOT NULL,
    -- Prevent an account from transferring money to itself
    CONSTRAINT chk_distinct_accounts CHECK (debit_account <> credit_account)
);

-- Optimize queries for financial statements and reporting
CREATE INDEX idx_transactions_debit ON transactions(debit_account);
CREATE INDEX idx_transactions_credit ON transactions(credit_account);

