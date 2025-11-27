package resultset

import (
"bytes"
"io"

"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/types"
"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/utils"
)

// ParseColumnDefinition 解析列定义包
func ParseColumnDefinition(payload []byte) (*types.ColumnDefinition, error) {
r := bytes.NewReader(payload)
col := &types.ColumnDefinition{}

// catalog (length-encoded string)
catalog, err := utils.ReadLengthEncodedString(r)
if err != nil {
return nil, err
}
col.Catalog = catalog

// schema (length-encoded string)
schema, err := utils.ReadLengthEncodedString(r)
if err != nil {
return nil, err
}
col.Schema = schema

// table (length-encoded string)
table, err := utils.ReadLengthEncodedString(r)
if err != nil {
return nil, err
}
col.Table = table

// org_table (length-encoded string)
orgTable, err := utils.ReadLengthEncodedString(r)
if err != nil {
return nil, err
}
col.OrgTable = orgTable

// name (length-encoded string)
name, err := utils.ReadLengthEncodedString(r)
if err != nil {
return nil, err
}
col.Name = name

// org_name (length-encoded string)
orgName, err := utils.ReadLengthEncodedString(r)
if err != nil {
return nil, err
}
col.OrgName = orgName

// fixed_length (length-encoded integer)
fixedLength, err := utils.ReadLengthEncodedInteger(r)
if err != nil {
return nil, err
}
col.FixedLength = fixedLength

// character_set (2字节)
charset, err := utils.ReadUint16(r)
if err != nil {
return nil, err
}
col.CharacterSet = charset

// column_length (4字节)
colLength, err := utils.ReadUint32(r)
if err != nil {
return nil, err
}
col.ColumnLength = colLength

// type (1字节)
colType, err := utils.ReadUint8(r)
if err != nil {
return nil, err
}
col.ColumnType = colType

// flags (2字节)
flags, err := utils.ReadUint16(r)
if err != nil {
return nil, err
}
col.Flags = flags

// decimals (1字节)
decimals, err := utils.ReadUint8(r)
if err != nil {
return nil, err
}
col.Decimals = decimals

// filler (2字节, 0x00 0x00)
if _, err := utils.ReadUint16(r); err != nil && err != io.EOF {
return nil, err
}

return col, nil
}

// BuildColumnDefinition 构建列定义包
func BuildColumnDefinition(col *types.ColumnDefinition) ([]byte, error) {
var buf bytes.Buffer

// catalog
if err := utils.WriteLengthEncodedString(&buf, col.Catalog); err != nil {
return nil, err
}

// schema
if err := utils.WriteLengthEncodedString(&buf, col.Schema); err != nil {
return nil, err
}

// table
if err := utils.WriteLengthEncodedString(&buf, col.Table); err != nil {
return nil, err
}

// org_table
if err := utils.WriteLengthEncodedString(&buf, col.OrgTable); err != nil {
return nil, err
}

// name
if err := utils.WriteLengthEncodedString(&buf, col.Name); err != nil {
return nil, err
}

// org_name
if err := utils.WriteLengthEncodedString(&buf, col.OrgName); err != nil {
return nil, err
}

// fixed_length
if err := utils.WriteLengthEncodedInteger(&buf, col.FixedLength); err != nil {
return nil, err
}

// character_set
if err := utils.WriteUint16(&buf, col.CharacterSet); err != nil {
return nil, err
}

// column_length
if err := utils.WriteUint32(&buf, col.ColumnLength); err != nil {
return nil, err
}

// type
if err := utils.WriteUint8(&buf, col.ColumnType); err != nil {
return nil, err
}

// flags
if err := utils.WriteUint16(&buf, col.Flags); err != nil {
return nil, err
}

// decimals
if err := utils.WriteUint8(&buf, col.Decimals); err != nil {
return nil, err
}

// filler
if err := utils.WriteUint16(&buf, 0); err != nil {
return nil, err
}

return buf.Bytes(), nil
}

// ParseTextResultSetRow 解析文本协议结果集行
func ParseTextResultSetRow(payload []byte, columnCount int) (*types.ResultSetRow, error) {
r := bytes.NewReader(payload)
row := &types.ResultSetRow{
Values: make([]interface{}, columnCount),
}

for i := 0; i < columnCount; i++ {
// 每个值都是length-encoded string
// 如果是NULL，第一个字节是0xFB
firstByte, err := r.ReadByte()
if err != nil {
return nil, err
}

if firstByte == 0xFB {
// NULL值
row.Values[i] = nil
} else {
// 回退一个字节
if err := r.UnreadByte(); err != nil {
return nil, err
}

// 读取length-encoded string
value, err := utils.ReadLengthEncodedString(r)
if err != nil {
return nil, err
}
row.Values[i] = value
}
}

return row, nil
}

// BuildTextResultSetRow 构建文本协议结果集行
func BuildTextResultSetRow(row *types.ResultSetRow) ([]byte, error) {
var buf bytes.Buffer

for _, value := range row.Values {
if value == nil {
// NULL值
if err := utils.WriteUint8(&buf, 0xFB); err != nil {
return nil, err
}
} else {
// 转换为字符串
var strValue string
switch v := value.(type) {
case string:
strValue = v
case []byte:
strValue = string(v)
default:
// 其他类型转换为字符串（这里简化处理）
strValue = ""
}

// 写入length-encoded string
if err := utils.WriteLengthEncodedString(&buf, strValue); err != nil {
return nil, err
}
}
}

return buf.Bytes(), nil
}
