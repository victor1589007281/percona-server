package factory

import (
"fmt"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/fsp"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/index"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/inode"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/lob"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/rseg"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/sdi"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/trxsys"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/undo"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/xdes"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// ParsedPage 通用解析后的Page接口
type ParsedPage interface {
GetPage() *types.Page
GetPageType() uint16
GetTypeName() string
}

// GenericParsedPage 通用Page结构
type GenericParsedPage struct {
Page     *types.Page
PageType uint16
TypeName string
Specific interface{} // 具体类型的解析结果
}

func (g *GenericParsedPage) GetPage() *types.Page {
return g.Page
}

func (g *GenericParsedPage) GetPageType() uint16 {
return g.PageType
}

func (g *GenericParsedPage) GetTypeName() string {
return g.TypeName
}

// ParsePage 根据Page类型自动解析
func ParsePage(page *types.Page) (ParsedPage, error) {
result := &GenericParsedPage{
Page:     page,
PageType: page.PageType,
TypeName: GetPageTypeName(page.PageType),
}

var err error

switch page.PageType {
case types.FIL_PAGE_TYPE_FSP_HDR:
result.Specific, err = fsp.ParseFSPHeader(page)

case types.FIL_PAGE_INDEX:
result.Specific, err = index.ParseIndexPage(page)

case types.FIL_PAGE_UNDO_LOG:
result.Specific, err = undo.ParseUndoPage(page)

case types.FIL_PAGE_INODE:
result.Specific, err = inode.ParseInodePage(page)

case types.FIL_PAGE_TYPE_XDES:
result.Specific, err = xdes.ParseXDESPage(page)

case types.FIL_PAGE_TYPE_TRX_SYS:
result.Specific, err = trxsys.ParseTRXSysPage(page)

case types.FIL_PAGE_TYPE_RSEG_ARRAY:
result.Specific, err = rseg.ParseRSEGArrayPage(page)

case types.FIL_PAGE_TYPE_LOB_FIRST, types.FIL_PAGE_TYPE_ZLOB_FIRST:
result.Specific, err = lob.ParseLOBFirstPage(page)

case types.FIL_PAGE_TYPE_LOB_DATA, types.FIL_PAGE_TYPE_ZLOB_DATA:
result.Specific, err = lob.ParseLOBDataPage(page)

case types.FIL_PAGE_TYPE_LOB_INDEX, types.FIL_PAGE_TYPE_ZLOB_INDEX:
result.Specific, err = lob.ParseLOBIndexPage(page)

case types.FIL_PAGE_SDI:
result.Specific, err = sdi.ParseSDIPage(page)

default:
// 未知或未实现的Page类型，返回基本Page
result.Specific = nil
err = nil
}

return result, err
}

// GetPageTypeName 获取Page类型名称
func GetPageTypeName(pageType uint16) string {
switch pageType {
case types.FIL_PAGE_INDEX:
return "INDEX"
case types.FIL_PAGE_UNDO_LOG:
return "UNDO_LOG"
case types.FIL_PAGE_INODE:
return "INODE"
case types.FIL_PAGE_IBUF_FREE_LIST:
return "IBUF_FREE_LIST"
case types.FIL_PAGE_TYPE_ALLOCATED:
return "ALLOCATED"
case types.FIL_PAGE_IBUF_BITMAP:
return "IBUF_BITMAP"
case types.FIL_PAGE_TYPE_SYS:
return "SYS"
case types.FIL_PAGE_TYPE_TRX_SYS:
return "TRX_SYS"
case types.FIL_PAGE_TYPE_FSP_HDR:
return "FSP_HDR"
case types.FIL_PAGE_TYPE_XDES:
return "XDES"
case types.FIL_PAGE_TYPE_BLOB:
return "BLOB"
case types.FIL_PAGE_SDI:
return "SDI"
case types.FIL_PAGE_TYPE_RSEG_ARRAY:
return "RSEG_ARRAY"
case types.FIL_PAGE_TYPE_LOB_INDEX:
return "LOB_INDEX"
case types.FIL_PAGE_TYPE_LOB_DATA:
return "LOB_DATA"
case types.FIL_PAGE_TYPE_LOB_FIRST:
return "LOB_FIRST"
case types.FIL_PAGE_TYPE_ZLOB_FIRST:
return "ZLOB_FIRST"
case types.FIL_PAGE_TYPE_ZLOB_DATA:
return "ZLOB_DATA"
case types.FIL_PAGE_TYPE_ZLOB_INDEX:
return "ZLOB_INDEX"
case types.FIL_PAGE_TYPE_ZLOB_FRAG:
return "ZLOB_FRAG"
default:
return fmt.Sprintf("UNKNOWN(0x%04X)", pageType)
}
}

// IsValidPageType 检查是否为有效的Page类型
func IsValidPageType(pageType uint16) bool {
switch pageType {
case types.FIL_PAGE_TYPE_ALLOCATED,
types.FIL_PAGE_UNDO_LOG,
types.FIL_PAGE_INODE,
types.FIL_PAGE_IBUF_FREE_LIST,
types.FIL_PAGE_IBUF_BITMAP,
types.FIL_PAGE_TYPE_SYS,
types.FIL_PAGE_TYPE_TRX_SYS,
types.FIL_PAGE_TYPE_FSP_HDR,
types.FIL_PAGE_TYPE_XDES,
types.FIL_PAGE_TYPE_BLOB,
types.FIL_PAGE_SDI,
types.FIL_PAGE_INDEX,
types.FIL_PAGE_TYPE_RSEG_ARRAY,
types.FIL_PAGE_TYPE_LOB_INDEX,
types.FIL_PAGE_TYPE_LOB_DATA,
types.FIL_PAGE_TYPE_LOB_FIRST,
types.FIL_PAGE_TYPE_ZLOB_FIRST,
types.FIL_PAGE_TYPE_ZLOB_DATA,
types.FIL_PAGE_TYPE_ZLOB_INDEX,
types.FIL_PAGE_TYPE_ZLOB_FRAG:
return true
default:
return false
}
}

// GetSpecificPage 获取具体类型的Page
func GetSpecificPage(parsed ParsedPage, targetType interface{}) error {
generic, ok := parsed.(*GenericParsedPage)
if !ok {
return types.NewError(1, "Invalid parsed page type")
}

if generic.Specific == nil {
return types.NewError(1, "No specific page data available")
}

// 使用类型断言将Specific转换为目标类型
switch target := targetType.(type) {
case **types.IndexPage:
if indexPage, ok := generic.Specific.(*types.IndexPage); ok {
*target = indexPage
return nil
}
case **types.FSPHeader:
if fspHeader, ok := generic.Specific.(*types.FSPHeader); ok {
*target = fspHeader
return nil
}
case **types.UndoPage:
if undoPage, ok := generic.Specific.(*types.UndoPage); ok {
*target = undoPage
return nil
}
case **types.InodePage:
if inodePage, ok := generic.Specific.(*types.InodePage); ok {
*target = inodePage
return nil
}
case **types.XDESPage:
if xdesPage, ok := generic.Specific.(*types.XDESPage); ok {
*target = xdesPage
return nil
}
case **types.TRXSysPage:
if trxsysPage, ok := generic.Specific.(*types.TRXSysPage); ok {
*target = trxsysPage
return nil
}
case **types.RSEGArrayPage:
if rsegPage, ok := generic.Specific.(*types.RSEGArrayPage); ok {
*target = rsegPage
return nil
}
case **types.LOBFirstPage:
if lobFirst, ok := generic.Specific.(*types.LOBFirstPage); ok {
*target = lobFirst
return nil
}
case **types.LOBDataPage:
if lobData, ok := generic.Specific.(*types.LOBDataPage); ok {
*target = lobData
return nil
}
case **types.LOBIndexPage:
if lobIndex, ok := generic.Specific.(*types.LOBIndexPage); ok {
*target = lobIndex
return nil
}
case **types.SDIPage:
if sdiPage, ok := generic.Specific.(*types.SDIPage); ok {
*target = sdiPage
return nil
}
}

return types.NewError(1, "Type mismatch")
}
