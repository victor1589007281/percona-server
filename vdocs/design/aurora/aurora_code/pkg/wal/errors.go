package wal

import "errors"

var (
	// ErrInvalidMagic indicates the magic number is invalid
	ErrInvalidMagic = errors.New("invalid WAL magic number")

	// ErrInvalidHeader indicates the header is invalid
	ErrInvalidHeader = errors.New("invalid WAL header")

	// ErrChecksumMismatch indicates checksum verification failed
	ErrChecksumMismatch = errors.New("checksum mismatch")

	// ErrWALFull indicates the WAL file is full
	ErrWALFull = errors.New("WAL file is full")

	// ErrWALClosed indicates the WAL file is closed
	ErrWALClosed = errors.New("WAL file is closed")

	// ErrInvalidLSN indicates the LSN is invalid
	ErrInvalidLSN = errors.New("invalid LSN")

	// ErrRecordTooLarge indicates the record is too large
	ErrRecordTooLarge = errors.New("record too large")

	// ErrFileNotFound indicates the WAL file was not found
	ErrFileNotFound = errors.New("WAL file not found")

	// ErrCorruptedFile indicates the WAL file is corrupted
	ErrCorruptedFile = errors.New("WAL file is corrupted")
)
