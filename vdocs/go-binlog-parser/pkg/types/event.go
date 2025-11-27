package types

const (
	QUERY_EVENT = 2
	ROTATE_EVENT = 4
	FORMAT_DESCRIPTION_EVENT = 15
	XID_EVENT = 16
	TABLE_MAP_EVENT = 19
	WRITE_ROWS_EVENTv2 = 30
	UPDATE_ROWS_EVENTv2 = 31
	DELETE_ROWS_EVENTv2 = 32
	GTID_EVENT = 33
)

type EventHeader struct {
	Timestamp uint32
	EventType uint8
	ServerID  uint32
	EventSize uint32
	LogPos    uint32
	Flags     uint16
}

type Event struct {
	Header *EventHeader
	Data   []byte
}

type QueryEvent struct {
	Database string
	Query    string
}

type GTIDEvent struct {
	GTID string
}

type XIDEvent struct {
	XID uint64
}
