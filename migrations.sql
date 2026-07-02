create database accounting;
\c accounting

create table accounts (
	id bigserial primary key,
	name varchar(64) not null unique,
	type integer not null
);

CREATE TABLE transactions (
  id BIGSERIAL PRIMARY KEY,
  amount NUMERIC(10, 2) NOT NULL CHECK (amount > 0.00),
  note VARCHAR(256) NOT NULL,
  
  debit_account_id bigint not null references accounts(id),
  credit_account_id bigint not null references accounts(id),
  created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP NOT NULL,
  -- Prevent an account from transferring money to itself
  constraint chk_distinct_account_ids CHECK (debit_account_id <> credit_account_id)
);
