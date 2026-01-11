// Package redo implements redo log parsing and processing
package redo

import (
	"encoding/binary"
	"errors"

	"github.com/percona/aurora/pkg/wal"
)

// Errors
var (
	ErrInvalidRedoType = errors.New("invalid redo type")
	ErrInvalidRedoData = errors.New("invalid redo data")
)

// InsertRedo represents an INSERT redo record
type InsertRedo struct {
	SlotNo    uint16
	RecordLen uint16
	RecordData []byte
}

// UpdateRedo represents an UPDATE redo record
type UpdateRedo struct {
	SlotNo   uint16
	Offset   uint16
	OldLen   uint16
	NewLen   uint16
	OldData  []byte
	NewData  []byte
}

// DeleteRedo represents a DELETE redo record
type DeleteRedo struct {
	SlotNo    uint16
	RecordLen uint16
	RecordData []byte
}

// ParseInsert parses INSERT redo data
func ParseInsert(data []byte) (*InsertRedo, error) {
	if len(data) < 4 {
		return nil, ErrInvalidRedoData
	}

	slotNo := binary.LittleEndian.Uint16(data[0:2])
	recLen := binary.LittleEndian.Uint16(data[2:4])

	if len(data) < 4+int(recLen) {
		return nil, ErrInvalidRedoData
	}

	return &InsertRedo{
		SlotNo:    slotNo,
		RecordLen: recLen,
		RecordData: data[4 : 4+recLen],
	}, nil
}

// ParseUpdate parses UPDATE redo data
func ParseUpdate(data []byte) (*UpdateRedo, error) {
	if len(data) < 8 {
		return nil, ErrInvalidRedoData
	}

	slotNo := binary.LittleEndian.Uint16(data[0:2])
	offset := binary.LittleEndian.Uint16(data[2:4])
	oldLen := binary.LittleEndian.Uint16(data[4:6])
	newLen := binary.LittleEndian.Uint16(data[6:8])

	if len(data) < 8+int(oldLen)+int(newLen) {
		return nil, ErrInvalidRedoData
	}

	return &UpdateRedo{
		SlotNo:  slotNo,
		Offset:  offset,
		OldLen:  oldLen,
		NewLen:  newLen,
		OldData: data[8 : 8+oldLen],
		NewData: data[8+oldLen : 8+oldLen+newLen],
	}, nil
}

// ParseDelete parses DELETE redo data
func ParseDelete(data []byte) (*DeleteRedo, error) {
	if len(data) < 4 {
		return nil, ErrInvalidRedoData
	}

	slotNo := binary.LittleEndian.Uint16(data[0:2])
	recLen := binary.LittleEndian.Uint16(data[2:4])

	if len(data) < 4+int(recLen) {
		return nil, ErrInvalidRedoData
	}

	return &DeleteRedo{
		SlotNo:    slotNo,
		RecordLen: recLen,
		RecordData: data[4 : 4+recLen],
	}, nil
}

// Encode serializes InsertRedo to bytes
func (r *InsertRedo) Encode() []byte {
	buf := make([]byte, 4+len(r.RecordData))
	binary.LittleEndian.PutUint16(buf[0:2], r.SlotNo)
	binary.LittleEndian.PutUint16(buf[2:4], r.RecordLen)
	copy(buf[4:], r.RecordData)
	return buf
}

// Encode serializes UpdateRedo to bytes
func (r *UpdateRedo) Encode() []byte {
	buf := make([]byte, 8+len(r.OldData)+len(r.NewData))
	binary.LittleEndian.PutUint16(buf[0:2], r.SlotNo)
	binary.LittleEndian.PutUint16(buf[2:4], r.Offset)
	binary.LittleEndian.PutUint16(buf[4:6], r.OldLen)
	binary.LittleEndian.PutUint16(buf[6:8], r.NewLen)
	copy(buf[8:], r.OldData)
	copy(buf[8+len(r.OldData):], r.NewData)
	return buf
}

// Encode serializes DeleteRedo to bytes
func (r *DeleteRedo) Encode() []byte {
	buf := make([]byte, 4+len(r.RecordData))
	binary.LittleEndian.PutUint16(buf[0:2], r.SlotNo)
	binary.LittleEndian.PutUint16(buf[2:4], r.RecordLen)
	copy(buf[4:], r.RecordData)
	return buf
}

// NewRedoRecord creates a new WAL redo record
func NewRedoRecord(lsn, spaceID, pageID, trxID, mtrID uint64, redoType wal.RedoType, flags uint16, data []byte) *wal.RedoRecord {
	return &wal.RedoRecord{
		Header: wal.RedoRecordHeader{
			LSN:     lsn,
			SpaceID: spaceID,
			PageID:  pageID,
			TrxID:   trxID,
			MtrID:   mtrID,
			Type:    redoType,
			Flags:   flags,
			DataLen: uint32(len(data)),
		},
		Data: data,
	}
}

// ParseRedoRecord parses the data portion of a redo record based on type
func ParseRedoRecord(record *wal.RedoRecord) (interface{}, error) {
	switch record.Header.Type {
	case wal.RedoTypeInsert:
		return ParseInsert(record.Data)
	case wal.RedoTypeUpdate:
		return ParseUpdate(record.Data)
	case wal.RedoTypeDelete:
		return ParseDelete(record.Data)
	case wal.RedoTypeTrxCommit, wal.RedoTypeTrxRollback:
		return nil, nil // No additional data
	default:
		return nil, ErrInvalidRedoType
	}
}
