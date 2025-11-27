package binlog

import (
	"os"
	"testing"
)

func TestParserCreate(t *testing.T) {
	tmpDir := t.TempDir()
	testFile := tmpDir + "/test.binlog"
	
	// Create test binlog file
	file, err := os.Create(testFile)
	if err != nil {
		t.Fatal(err)
	}
	
	// Write magic
	file.WriteString(BinlogMagic)
	file.Close()
	
	// Test parser
	parser, err := NewParser(testFile)
	if err != nil {
		t.Fatal(err)
	}
	defer parser.Close()
	
	if parser == nil {
		t.Error("expected parser to be created")
	}
}

func TestParserInvalidMagic(t *testing.T) {
	tmpDir := t.TempDir()
	testFile := tmpDir + "/invalid.binlog"
	
	file, _ := os.Create(testFile)
	file.WriteString("INVALID")
	file.Close()
	
	_, err := NewParser(testFile)
	if err == nil {
		t.Error("expected error for invalid magic")
	}
}
