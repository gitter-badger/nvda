/*
This file is a part of the NVDA project.
URL: http://www.nvda-project.org/
Copyright 2006-2010 NVDA contributers.
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2.0, as published by
    the Free Software Foundation.
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
This license can be found at:
http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
*/

#define WIN32_LEAN_AND_MEAN 

#include <sstream>
#include <vector>
#include <comdef.h>
#include <windows.h>
#include <oleacc.h>
#include <common/xml.h>
#include <common/log.h>
#include <boost/optional.hpp>
#include "nvdaHelperRemote.h"
#include "nvdaInProcUtils.h"
#include "nvdaInProcUtils.h"
#include <remote/WinWord/Constants.h>
#include <remote/WinWord/Fields.h>
#include "winword.h"

using namespace std;

// See https://github.com/nvaccess/nvda/wiki/Using-COM-with-NVDA-and-Microsoft-Word
constexpr int formatConfig_reportFontName = 0x1;
constexpr int formatConfig_reportFontSize = 0x2;
constexpr int formatConfig_reportFontAttributes = 0x4;
constexpr int formatConfig_reportColor = 0x8;
constexpr int formatConfig_reportAlignment = 0x10;
constexpr int formatConfig_reportStyle = 0x20;
constexpr int formatConfig_reportSpellingErrors = 0x40;
constexpr int formatConfig_reportPage = 0x80;
constexpr int formatConfig_reportLineNumber = 0x100;
constexpr int formatConfig_reportTables = 0x200;
constexpr int formatConfig_reportLists = 0x400;
constexpr int formatConfig_reportLinks = 0x800;
constexpr int formatConfig_reportComments = 0x1000;
constexpr int formatConfig_reportHeadings = 0x2000;
constexpr int formatConfig_reportLanguage = 0x4000;
constexpr int formatConfig_reportRevisions = 0x8000;
constexpr int formatConfig_reportParagraphIndentation = 0x10000;
constexpr int formatConfig_includeLayoutTables = 0x20000;
constexpr int formatConfig_reportLineSpacing = 0x40000;

constexpr int formatConfig_fontFlags =(formatConfig_reportFontName|formatConfig_reportFontSize|formatConfig_reportFontAttributes|formatConfig_reportColor);
constexpr int formatConfig_initialFormatFlags =(formatConfig_reportPage|formatConfig_reportLineNumber|formatConfig_reportTables|formatConfig_reportHeadings|formatConfig_includeLayoutTables);

constexpr wchar_t PAGE_BREAK_VALUE = L'\x0c';
constexpr wchar_t COLUMN_BREAK_VALUE = L'\x0e';

UINT wm_winword_expandToLine=0;
typedef struct {
	int offset;
	int lineStart;
	int lineEnd;
} winword_expandToLine_args;
void winword_expandToLine_helper(HWND hwnd, winword_expandToLine_args* args) {
	//Fetch all needed objects
	IDispatchPtr pDispatchWindow=NULL;
	if(AccessibleObjectFromWindow(hwnd,OBJID_NATIVEOM,IID_IDispatch,(void**)&pDispatchWindow)!=S_OK||!pDispatchWindow) {
		LOG_DEBUGWARNING(L"AccessibleObjectFromWindow failed");
		return;
	}
	IDispatchPtr pDispatchApplication=NULL;
	if(_com_dispatch_raw_propget(pDispatchWindow,wdDISPID_WINDOW_APPLICATION,VT_DISPATCH,&pDispatchApplication)!=S_OK||!pDispatchApplication) {
		LOG_DEBUGWARNING(L"window.application failed");
		return;
	}
	IDispatchPtr pDispatchSelection=NULL;
	if(_com_dispatch_raw_propget(pDispatchWindow,wdDISPID_WINDOW_SELECTION,VT_DISPATCH,&pDispatchSelection)!=S_OK||!pDispatchSelection) {
		LOG_DEBUGWARNING(L"application.selection failed");
		return;
	}
	BOOL startWasActive=false;
	if(_com_dispatch_raw_propget(pDispatchSelection,wdDISPID_SELECTION_STARTISACTIVE,VT_BOOL,&startWasActive)!=S_OK) {
		LOG_DEBUGWARNING(L"selection.StartIsActive failed");
	}
	IDispatch* pDispatchOldSelRange=NULL;
	if(_com_dispatch_raw_propget(pDispatchSelection,wdDISPID_SELECTION_RANGE,VT_DISPATCH,&pDispatchOldSelRange)!=S_OK||!pDispatchOldSelRange) {
		LOG_DEBUGWARNING(L"selection.range failed");
		return;
	}
	//Disable screen updating as we will be moving the selection temporarily
	_com_dispatch_raw_propput(pDispatchApplication,wdDISPID_APPLICATION_SCREENUPDATING,VT_BOOL,false);
	//Move the selection to the given range
	_com_dispatch_raw_method(pDispatchSelection,wdDISPID_SELECTION_SETRANGE,DISPATCH_METHOD,VT_EMPTY,NULL,L"\x0003\x0003",args->offset,args->offset);
	//Expand the selection to the line
	// #3421: Expand and or extending selection cannot be used due to MS Word bugs on the last line in a table cell, or the first/last line of a table of contents, selecting would select the entire object.  
	// Therefore do it in two steps
	bool lineError=false;
	if(_com_dispatch_raw_method(pDispatchSelection,wdDISPID_SELECTION_STARTOF,DISPATCH_METHOD,VT_EMPTY,NULL,L"\x0003\x0003",wdLine,0)!=S_OK) {
		lineError=true;
	} else {
		_com_dispatch_raw_propget(pDispatchSelection,wdDISPID_RANGE_START,VT_I4,&(args->lineStart));
		if(_com_dispatch_raw_method(pDispatchSelection,wdDISPID_SELECTION_ENDOF,DISPATCH_METHOD,VT_EMPTY,NULL,L"\x0003\x0003",wdLine,0)!=S_OK) {
			lineError=true;
		} else {
			_com_dispatch_raw_propget(pDispatchSelection,wdDISPID_RANGE_END,VT_I4,&(args->lineEnd));
		}
		// the endOf method has a bug where IPAtEndOfLine gets stuck as true on wrapped lines
		// So reset the selection to the start of the document to force it to False 
		_com_dispatch_raw_method(pDispatchSelection,wdDISPID_SELECTION_SETRANGE,DISPATCH_METHOD,VT_EMPTY,NULL,L"\x0003\x0003",0,0);
	}
	// Fall back to the older expand if there was an error getting line bounds
	if(lineError) {
		_com_dispatch_raw_method(pDispatchSelection,wdDISPID_SELECTION_SETRANGE,DISPATCH_METHOD,VT_EMPTY,NULL,L"\x0003\x0003",args->offset,args->offset);
		_com_dispatch_raw_method(pDispatchSelection,wdDISPID_RANGE_EXPAND,DISPATCH_METHOD,VT_EMPTY,NULL,L"\x0003",wdLine);
		_com_dispatch_raw_propget(pDispatchSelection,wdDISPID_RANGE_START,VT_I4,&(args->lineStart));
		_com_dispatch_raw_propget(pDispatchSelection,wdDISPID_RANGE_END,VT_I4,&(args->lineEnd));
	} 
	if(args->lineStart>=args->lineEnd) {
		args->lineStart=args->offset;
		args->lineEnd=args->offset+1;
	}
	//Move the selection back to its original location
	_com_dispatch_raw_method(pDispatchOldSelRange,wdDISPID_RANGE_SELECT,DISPATCH_METHOD,VT_EMPTY,NULL,NULL);
	//Restore the old selection direction
	_com_dispatch_raw_propput(pDispatchSelection,wdDISPID_SELECTION_STARTISACTIVE,VT_BOOL,startWasActive);
	//Reenable screen updating
	_com_dispatch_raw_propput(pDispatchApplication,wdDISPID_APPLICATION_SCREENUPDATING,VT_BOOL,true);
}

BOOL generateFormFieldXML(IDispatch* pDispatchRange, IDispatchPtr pDispatchRangeExpandedToParagraph, wostringstream& XMLStream, int& chunkEnd) {
	IDispatchPtr pDispatchRange2=pDispatchRangeExpandedToParagraph;
	BOOL foundFormField=false;
	IDispatchPtr pDispatchFormFields=NULL;
	_com_dispatch_raw_propget(pDispatchRange2,wdDISPID_RANGE_FORMFIELDS,VT_DISPATCH,&pDispatchFormFields);
	if(pDispatchFormFields) for(int count=1;!foundFormField&&count<100;++count) {
		IDispatchPtr pDispatchFormField=NULL;
		if(_com_dispatch_raw_method(pDispatchFormFields,wdDISPID_FORMFIELDS_ITEM,DISPATCH_METHOD,VT_DISPATCH,&pDispatchFormField,L"\x0003",count)!=S_OK||!pDispatchFormField) {
			break;
		}
		IDispatchPtr pDispatchFormFieldRange=NULL;
		if(_com_dispatch_raw_propget(pDispatchFormField,wdDISPID_FORMFIELD_RANGE,VT_DISPATCH,&pDispatchFormFieldRange)!=S_OK||!pDispatchFormFieldRange) {
			break;
		}
		if(_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_INRANGE,DISPATCH_METHOD,VT_BOOL,&foundFormField,L"\x0009",pDispatchFormFieldRange)!=S_OK||!foundFormField) {
			continue;
		}
		long fieldType=-1;
		_com_dispatch_raw_propget(pDispatchFormField,wdDISPID_FORMFIELD_TYPE,VT_I4,&fieldType);
		BSTR fieldResult=NULL;
		_com_dispatch_raw_propget(pDispatchFormField,wdDISPID_FORMFIELD_RESULT,VT_BSTR,&fieldResult);
		BSTR fieldStatusText=NULL;
		_com_dispatch_raw_propget(pDispatchFormField,wdDISPID_FORMFIELD_STATUSTEXT,VT_BSTR,&fieldStatusText);
		XMLStream<<L"<control wdFieldType=\""<<fieldType<<L"\" wdFieldResult=\""<<(fieldResult?fieldResult:L"")<<L"\" wdFieldStatusText=\""<<(fieldStatusText?fieldStatusText:L"")<<L"\">";
		if(fieldResult) SysFreeString(fieldResult);
		if(fieldStatusText) SysFreeString(fieldStatusText);
		_com_dispatch_raw_propget(pDispatchFormFieldRange,wdDISPID_RANGE_END,VT_I4,&chunkEnd);
		_com_dispatch_raw_propput(pDispatchRange,wdDISPID_RANGE_END,VT_I4,chunkEnd);
		break;
	}
	if(foundFormField) return true;
	IDispatchPtr pDispatchContentControls=NULL;
	_com_dispatch_raw_propget(pDispatchRange2,wdDISPID_RANGE_CONTENTCONTROLS,VT_DISPATCH,&pDispatchContentControls);
	if(pDispatchContentControls)for(int count=1;!foundFormField&&count<100;++count) {
		IDispatchPtr pDispatchContentControl=NULL;
		if(_com_dispatch_raw_method(pDispatchContentControls,wdDISPID_CONTENTCONTROLS_ITEM,DISPATCH_METHOD,VT_DISPATCH,&pDispatchContentControl,L"\x0003",count)!=S_OK||!pDispatchContentControl) {
			break;
		}
		IDispatchPtr pDispatchContentControlRange=NULL;
		if(_com_dispatch_raw_propget(pDispatchContentControl,wdDISPID_CONTENTCONTROL_RANGE,VT_DISPATCH,&pDispatchContentControlRange)!=S_OK||!pDispatchContentControlRange) {
			break;
		}
		if(_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_INRANGE,DISPATCH_METHOD,VT_BOOL,&foundFormField,L"\x0009",pDispatchContentControlRange)!=S_OK||!foundFormField) {
			continue;
		}
		long fieldType=-1;
		_com_dispatch_raw_propget(pDispatchContentControl,wdDISPID_CONTENTCONTROL_TYPE,VT_I4,&fieldType);
		BOOL fieldChecked=false;
		_com_dispatch_raw_propget(pDispatchContentControl,wdDISPID_CONTENTCONTROL_CHECKED,VT_BOOL,&fieldChecked);
		BSTR fieldTitle=NULL;
		_com_dispatch_raw_propget(pDispatchContentControl,wdDISPID_CONTENTCONTROL_TITLE,VT_BSTR,&fieldTitle);
		XMLStream<<L"<control wdContentControlType=\""<<fieldType<<L"\" wdContentControlChecked=\""<<fieldChecked<<L"\" wdContentControlTitle=\""<<(fieldTitle?fieldTitle:L"")<<L"\">";
		if(fieldTitle) SysFreeString(fieldTitle);
		_com_dispatch_raw_propget(pDispatchContentControlRange,wdDISPID_RANGE_END,VT_I4,&chunkEnd);
		_com_dispatch_raw_propput(pDispatchRange,wdDISPID_RANGE_END,VT_I4,chunkEnd);
		break;
	}
	return foundFormField;
}

bool collectSpellingErrorOffsets(IDispatchPtr pDispatchRange, vector<pair<long,long>>& errorVector) {
	IDispatchPtr pDispatchApplication=NULL;
	if(_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_APPLICATION ,VT_DISPATCH,&pDispatchApplication)!=S_OK||!pDispatchApplication) {
		return false;
	}
	BOOL isSandbox = false;
	// Don't go on if this is sandboxed as collecting spelling errors crashes word
	_com_dispatch_raw_propget(pDispatchApplication,wdDISPID_APPLICATION_ISSANDBOX ,VT_BOOL,&isSandbox);
	if(isSandbox ) {
		return false;
	}
	IDispatchPtr pDispatchSpellingErrors=NULL;
	if(_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_SPELLINGERRORS,VT_DISPATCH,&pDispatchSpellingErrors)!=S_OK||!pDispatchSpellingErrors) {
		return false;
	}
	long iVal=0;
	_com_dispatch_raw_propget(pDispatchSpellingErrors,wdDISPID_SPELLINGERRORS_COUNT,VT_I4,&iVal);
	for(int i=1;i<=iVal;++i) {
		IDispatchPtr pDispatchErrorRange=NULL;
		if(_com_dispatch_raw_method(pDispatchSpellingErrors,wdDISPID_SPELLINGERRORS_ITEM,DISPATCH_METHOD,VT_DISPATCH,&pDispatchErrorRange,L"\x0003",i)!=S_OK||!pDispatchErrorRange) {
			return false;
		}
		long start=0;
		if(_com_dispatch_raw_propget(pDispatchErrorRange,wdDISPID_RANGE_START,VT_I4,&start)!=S_OK) {
			return false;
		}
		long end=0;
		if(_com_dispatch_raw_propget(pDispatchErrorRange,wdDISPID_RANGE_END,VT_I4,&end)!=S_OK) {
			return false;
		}
		errorVector.push_back(make_pair(start,end));
	}
	return !errorVector.empty();
}

// #6033: This must not be a static variable inside the function
// because that causes crashes on Windows XP (Visual Studio bug 1941836).
vector<wstring> headingStyleNames;
int getHeadingLevelFromParagraph(IDispatch* pDispatchParagraph) {
	IDispatchPtr pDispatchStyle=NULL;
	// fetch the localized style name for the given paragraph
	if(_com_dispatch_raw_propget(pDispatchParagraph,wdDISPID_PARAGRAPH_STYLE,VT_DISPATCH,&pDispatchStyle)!=S_OK||!pDispatchStyle) {
		return 0;
	}
	BSTR nameLocal=NULL;
	_com_dispatch_raw_propget(pDispatchStyle,wdDISPID_STYLE_NAMELOCAL,VT_BSTR,&nameLocal);
	if(!nameLocal) {
		return 0;
	}
	// If not fetched already, fetch all builtin heading style localized names (1 through 9).
	if(headingStyleNames.empty()) {
		IDispatchPtr pDispatchDocument=NULL;
		IDispatchPtr pDispatchStyles=NULL;
		if(_com_dispatch_raw_propget(pDispatchStyle,wdDISPID_STYLE_PARENT,VT_DISPATCH,&pDispatchDocument)==S_OK&&pDispatchDocument&&_com_dispatch_raw_propget(pDispatchDocument,wdDISPID_DOCUMENT_STYLES,VT_DISPATCH,&pDispatchStyles)==S_OK&&pDispatchStyles) {
			for(int i=-2;i>=-10;--i) {
				IDispatchPtr pDispatchBuiltinStyle=NULL;
				_com_dispatch_raw_method(pDispatchStyles,wdDISPID_STYLES_ITEM,DISPATCH_METHOD,VT_DISPATCH,&pDispatchBuiltinStyle,L"\x0003",i);
				if(pDispatchBuiltinStyle) {
					BSTR builtinNameLocal=NULL;
					_com_dispatch_raw_propget(pDispatchBuiltinStyle,wdDISPID_STYLE_NAMELOCAL,VT_BSTR,&builtinNameLocal);
					if(!builtinNameLocal) continue;
					headingStyleNames.push_back(builtinNameLocal);
					SysFreeString(builtinNameLocal);
				}
			}
		}
	}
	int level=0;
	int count=1;
	// See if the style name matches one of the builtin heading styles
	for(auto i=headingStyleNames.cbegin();i!=headingStyleNames.cend();++i) {
		if(i->compare(nameLocal)==0) {
			level=count;
			break;
		}
		count+=1;
	}
	SysFreeString(nameLocal);
	return level;
}

int generateHeadingXML(IDispatch* pDispatchParagraph, IDispatch* pDispatchParagraphRange, int startOffset, int endOffset, wostringstream& XMLStream) {
	int headingLevel=getHeadingLevelFromParagraph(pDispatchParagraph);
	if(!headingLevel) return 0;
	XMLStream<<L"<control role=\"heading\" level=\""<<headingLevel<<L"\" ";
	if(pDispatchParagraphRange) {
		long iVal=0;
		if(_com_dispatch_raw_propget(pDispatchParagraphRange,wdDISPID_RANGE_START,VT_I4,&iVal)==S_OK&&iVal>=startOffset) {
			XMLStream<<L"_startOfNode=\"1\" ";
		}
		if(_com_dispatch_raw_propget(pDispatchParagraphRange,wdDISPID_RANGE_END,VT_I4,&iVal)==S_OK&&iVal<=endOffset) {
			XMLStream<<L"_endOfNode=\"1\" ";
		}
	}
	XMLStream<<L">";
	return 1;
}

int getRevisionType(IDispatch* pDispatchOrigRange) {
	IDispatchPtr pDispatchRange=NULL;
	//If range is not duplicated here, revisions collection represents revisions at the start of the range when it was first created
	if(_com_dispatch_raw_propget(pDispatchOrigRange,wdDISPID_RANGE_DUPLICATE,VT_DISPATCH,&pDispatchRange)!=S_OK||!pDispatchRange) {
		return 0;
	}
	IDispatchPtr pDispatchRevisions=NULL;
	if(_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_REVISIONS,VT_DISPATCH,&pDispatchRevisions)!=S_OK||!pDispatchRevisions) {
		return 0;
	}
	IDispatchPtr pDispatchRevision=NULL;
	if(_com_dispatch_raw_method(pDispatchRevisions,wdDISPID_REVISIONS_ITEM,DISPATCH_METHOD,VT_DISPATCH,&pDispatchRevision,L"\x0003",1)!=S_OK||!pDispatchRevision) {
		return 0;
	}
	long revisionType=0;
	_com_dispatch_raw_propget(pDispatchRevision,wdDISPID_REVISION_TYPE,VT_I4,&revisionType);
	return revisionType;
}

IDispatchPtr CreateExpandedDuplicate(IDispatch* pDispatchRange, const int expandTo) {
	IDispatchPtr pDispatchRangeDup = nullptr;
	auto res = _com_dispatch_raw_propget( pDispatchRange, wdDISPID_RANGE_DUPLICATE, VT_DISPATCH, &pDispatchRangeDup);
	if( res != S_OK || !pDispatchRangeDup ) {
		LOG_DEBUGWARNING(L"error duplicating the range.");
	}
	else {
		res = _com_dispatch_raw_method( pDispatchRangeDup, wdDISPID_RANGE_EXPAND,DISPATCH_METHOD,VT_EMPTY,NULL,L"\x0003", expandTo);
		if( res != S_OK || !pDispatchRangeDup ) {
			LOG_DEBUGWARNING(L"error expanding the range");
		}
	}
	return pDispatchRangeDup;
}

bool collectCommentOffsets(IDispatchPtr pDispatchRange, vector<pair<long,long>>& commentVector) {
	IDispatchPtr pDispatchComments=NULL;
	if(_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_COMMENTS,VT_DISPATCH,&pDispatchComments)!=S_OK||!pDispatchComments) {
		return false;
	}
	long iVal=0;
	_com_dispatch_raw_propget(pDispatchComments,wdDISPID_COMMENTS_COUNT,VT_I4,&iVal);
	for(int i=1;i<=iVal;++i) {
		IDispatchPtr pDispatchComment=NULL;
		if(_com_dispatch_raw_method(pDispatchComments,wdDISPID_COMMENTS_ITEM,DISPATCH_METHOD,VT_DISPATCH,&pDispatchComment,L"\x0003",i)!=S_OK||!pDispatchComment) {
			return false;
		}
		IDispatchPtr pDispatchCommentScope=NULL;
		if(_com_dispatch_raw_propget(pDispatchComment,wdDISPID_COMMENT_SCOPE,VT_DISPATCH,&pDispatchCommentScope)!=S_OK||!pDispatchCommentScope) {
			return false;
		}
		long start=0;
		if(_com_dispatch_raw_propget(pDispatchCommentScope,wdDISPID_RANGE_START,VT_I4,&start)!=S_OK) {
			return false;
		}
		long end=0;
		if(_com_dispatch_raw_propget(pDispatchCommentScope,wdDISPID_RANGE_END,VT_I4,&end)!=S_OK) {
			return false;
		}
		commentVector.push_back(make_pair(start,end));
	}
	return !commentVector.empty();
}

bool fetchTableInfo(IDispatch* pDispatchTable, bool includeLayoutTables, int* rowCount, int* columnCount, int* nestingLevel) {
	IDispatchPtr pDispatchRows=NULL;
	IDispatchPtr pDispatchColumns=NULL;
	IDispatchPtr pDispatchBorders=NULL;
	if(!includeLayoutTables&&_com_dispatch_raw_propget(pDispatchTable,wdDISPID_TABLE_BORDERS,VT_DISPATCH,&pDispatchBorders)==S_OK&&pDispatchBorders) {
		BOOL isEnabled=true;
		if(_com_dispatch_raw_propget(pDispatchBorders,wdDISPID_BORDERS_ENABLE,VT_BOOL,&isEnabled)==S_OK&&!isEnabled) {
			return false;
		}
	}
	if(_com_dispatch_raw_propget(pDispatchTable,wdDISPID_TABLE_ROWS,VT_DISPATCH,&pDispatchRows)==S_OK&&pDispatchRows) {
		_com_dispatch_raw_propget(pDispatchRows,wdDISPID_ROWS_COUNT,VT_I4,rowCount);
	}
	if(_com_dispatch_raw_propget(pDispatchTable,wdDISPID_TABLE_COLUMNS,VT_DISPATCH,&pDispatchColumns)==S_OK&&pDispatchColumns) {
		_com_dispatch_raw_propget(pDispatchColumns,wdDISPID_COLUMNS_COUNT,VT_I4,columnCount);
	}
	_com_dispatch_raw_propget(pDispatchTable,wdDISPID_TABLE_NESTINGLEVEL,VT_I4,nestingLevel);
	return true;
}

int generateTableXML(IDispatch* pDispatchRange, bool includeLayoutTables, int startOffset, int endOffset, wostringstream& XMLStream) {
	int numTags=0;
	int iVal=0;
	IDispatchPtr pDispatchTables=NULL;
	IDispatchPtr pDispatchTable=NULL;
	bool inTableCell=false;
	int rowCount=0;
	int columnCount=0;
	int nestingLevel=0;
	int rowNumber=0;
	int columnNumber=0;
	bool startOfCell=false;
	bool endOfCell=false;
	if(
		_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_TABLES,VT_DISPATCH,&pDispatchTables)!=S_OK||!pDispatchTables\
		||_com_dispatch_raw_method(pDispatchTables,wdDISPID_TABLES_ITEM,DISPATCH_METHOD,VT_DISPATCH,&pDispatchTable,L"\x0003",1)!=S_OK||!pDispatchTable\
		||!fetchTableInfo(pDispatchTable,includeLayoutTables,&rowCount,&columnCount,&nestingLevel)\
	) {
		return 0;
	}
	IDispatchPtr pDispatchCells=NULL;
	IDispatchPtr pDispatchCell=NULL;
	if(
		_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_CELLS,VT_DISPATCH,&pDispatchCells)==S_OK&&pDispatchCells\
		&&_com_dispatch_raw_method(pDispatchCells,wdDISPID_CELLS_ITEM,DISPATCH_METHOD,VT_DISPATCH,&pDispatchCell,L"\x0003",1)==S_OK&&pDispatchCell\
	) {
		_com_dispatch_raw_propget(pDispatchCell,wdDISPID_CELL_ROWINDEX,VT_I4,&rowNumber);
		_com_dispatch_raw_propget(pDispatchCell,wdDISPID_CELL_COLUMNINDEX,VT_I4,&columnNumber);
		IDispatchPtr pDispatchCellRange=NULL;
		if(_com_dispatch_raw_propget(pDispatchCell,wdDISPID_CELL_RANGE,VT_DISPATCH,&pDispatchCellRange)==S_OK&&pDispatchCellRange) {
			if(_com_dispatch_raw_propget(pDispatchCellRange,wdDISPID_RANGE_START,VT_I4,&iVal)==S_OK&&iVal>=startOffset) {
				startOfCell=true;
			}
			if(_com_dispatch_raw_propget(pDispatchCellRange,wdDISPID_RANGE_END,VT_I4,&iVal)==S_OK&&iVal<=endOffset) {
				endOfCell=true;
			}
		}
		inTableCell=true;
	} else {
		if((_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_INFORMATION,DISPATCH_PROPERTYGET,VT_I4,&rowNumber,L"\x0003",wdStartOfRangeRowNumber)==S_OK)&&rowNumber>0) {
			inTableCell=true;
		}
		if((_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_INFORMATION,DISPATCH_PROPERTYGET,VT_I4,&columnNumber,L"\x0003",wdStartOfRangeColumnNumber)==S_OK)&&columnNumber>0) {
			inTableCell=true;
		}
	}
	if(!inTableCell) return numTags;
	numTags+=2;
	XMLStream<<L"<control role=\"table\" table-id=\"1\" table-rowcount=\""<<rowCount<<L"\" table-columncount=\""<<columnCount<<L"\" level=\""<<nestingLevel<<L"\" ";
	wstring altTextStr=L"";
	BSTR altText=NULL;
	if(_com_dispatch_raw_propget(pDispatchTable,wdDISPID_TABLE_TITLE,VT_BSTR,&altText)==S_OK&&altText) {
		for(int i=0;altText[i]!='\0';++i) {
			appendCharToXML(altText[i],altTextStr,true);
		}
		SysFreeString(altText);
	}
	if(!altTextStr.empty()) {
		XMLStream<<L"alwaysReportName=\"1\" name=\""<<altTextStr<<L"\" ";
		altTextStr=L"";
	}
	altText=NULL;
	if(_com_dispatch_raw_propget(pDispatchTable,wdDISPID_TABLE_DESCR,VT_BSTR,&altText)==S_OK&&altText) {
		for(int i=0;altText[i]!='\0';++i) {
			appendCharToXML(altText[i],altTextStr,true);
		}
		XMLStream<<L"longdescription=\""<<altTextStr<<L"\" ";
		SysFreeString(altText);
	}
	IDispatchPtr pDispatchTableRange=NULL;
	if(_com_dispatch_raw_propget(pDispatchTable,wdDISPID_TABLE_RANGE,VT_DISPATCH,&pDispatchTableRange)==S_OK&&pDispatchTableRange) {
		if(_com_dispatch_raw_propget(pDispatchTableRange,wdDISPID_RANGE_START,VT_I4,&iVal)==S_OK&&iVal>=startOffset) {
			XMLStream<<L"_startOfNode=\"1\" ";
		}
		if(_com_dispatch_raw_propget(pDispatchTableRange,wdDISPID_RANGE_END,VT_I4,&iVal)==S_OK&&iVal<=endOffset) {
			XMLStream<<L"_endOfNode=\"1\" ";
		}
	}
	XMLStream<<L">";
	XMLStream<<L"<control role=\"tableCell\" table-id=\"1\" ";
	XMLStream<<L"table-rownumber=\""<<rowNumber<<L"\" ";
	XMLStream<<L"table-columnnumber=\""<<columnNumber<<L"\" ";
	if(startOfCell) {
		XMLStream<<L"_startOfNode=\"1\" ";
	}
	if(endOfCell) {
		XMLStream<<L"_endOfNode=\"1\" ";
	}
	XMLStream<<L">";
	return numTags;
}

void generateXMLAttribsForFormatting(IDispatch* pDispatchRange, int startOffset, int endOffset, int formatConfig, wostringstream& formatAttribsStream) {
	int iVal=0;
	if((formatConfig&formatConfig_reportPage)&&(_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_INFORMATION,DISPATCH_PROPERTYGET,VT_I4,&iVal,L"\x0003",wdActiveEndAdjustedPageNumber)==S_OK)&&iVal>0) {
		formatAttribsStream<<L"page-number=\""<<iVal<<L"\" ";
	}
	if((formatConfig&formatConfig_reportLineNumber)&&(_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_INFORMATION,DISPATCH_PROPERTYGET,VT_I4,&iVal,L"\x0003",wdFirstCharacterLineNumber)==S_OK)) {
		formatAttribsStream<<L"line-number=\""<<iVal<<L"\" ";
	}
	if((formatConfig&formatConfig_reportAlignment)||(formatConfig&formatConfig_reportParagraphIndentation)||(formatConfig&formatConfig_reportLineSpacing)) {
		IDispatchPtr pDispatchParagraphFormat=NULL;
		if(_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_PARAGRAPHFORMAT,VT_DISPATCH,&pDispatchParagraphFormat)==S_OK&&pDispatchParagraphFormat) {
			if(formatConfig&formatConfig_reportAlignment) {
				if(_com_dispatch_raw_propget(pDispatchParagraphFormat,wdDISPID_PARAGRAPHFORMAT_ALIGNMENT,VT_I4,&iVal)==S_OK) {
					switch(iVal) {
						case wdAlignParagraphLeft:
						formatAttribsStream<<L"text-align=\"left\" ";
						break;
						case wdAlignParagraphCenter:
						formatAttribsStream<<L"text-align=\"center\" ";
						break;
						case wdAlignParagraphRight:
						formatAttribsStream<<L"text-align=\"right\" ";
						break;
						case wdAlignParagraphJustify:
						formatAttribsStream<<L"text-align=\"justified\" ";
						break;
					}
				}
			}
			float fVal=0.0;
			if(formatConfig&formatConfig_reportParagraphIndentation) {
				if(_com_dispatch_raw_propget(pDispatchParagraphFormat,wdDISPID_PARAGRAPHFORMAT_RIGHTINDENT,VT_R4,&fVal)==S_OK) {
					formatAttribsStream<<L"right-indent=\"" << fVal <<L"\" ";
				}
				float firstLineIndent=0;
				if(_com_dispatch_raw_propget(pDispatchParagraphFormat,wdDISPID_PARAGRAPHFORMAT_FIRSTLINEINDENT,VT_R4,&firstLineIndent)==S_OK) {
					if(firstLineIndent<0) {
						formatAttribsStream<<L"hanging-indent=\"" << (0-firstLineIndent) <<L"\" ";
					} else {
						formatAttribsStream<<L"first-line-indent=\"" << firstLineIndent <<L"\" ";
					}
				}
				if(_com_dispatch_raw_propget(pDispatchParagraphFormat,wdDISPID_PARAGRAPHFORMAT_LEFTINDENT,VT_R4,&fVal)==S_OK) {
					if(firstLineIndent<0) fVal+=firstLineIndent;
					formatAttribsStream<<L"left-indent=\"" << fVal <<L"\" ";
				}
			}
			if(formatConfig&formatConfig_reportLineSpacing) {
				if(_com_dispatch_raw_propget(pDispatchParagraphFormat,wdDISPID_PARAGRAPHFORMAT_LINESPACINGRULE,VT_I4,&iVal)==S_OK) {
					formatAttribsStream<<L"wdLineSpacingRule=\"" << iVal <<L"\" ";
				}
				if(_com_dispatch_raw_propget(pDispatchParagraphFormat,wdDISPID_PARAGRAPHFORMAT_LINESPACING,VT_R4,&fVal)==S_OK) {
					formatAttribsStream<<L"wdLineSpacing=\"" << fVal <<L"\" ";
				}
			}
		}
	}
	if(formatConfig&formatConfig_reportLists) {
		IDispatchPtr pDispatchListFormat=NULL;
		if(_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_LISTFORMAT,VT_DISPATCH,&pDispatchListFormat)==S_OK&&pDispatchListFormat) {
			BSTR listString=NULL;
			if(_com_dispatch_raw_propget(pDispatchListFormat,wdDISPID_LISTFORMAT_LISTSTRING,VT_BSTR,&listString)==S_OK&&listString) {
				if(SysStringLen(listString)>0) {
					IDispatchPtr pDispatchParagraphs=NULL;
					IDispatchPtr pDispatchParagraph=NULL;
					IDispatchPtr pDispatchParagraphRange=NULL;
					if(
						_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_PARAGRAPHS,VT_DISPATCH,&pDispatchParagraphs)==S_OK&&pDispatchParagraphs\
						&&_com_dispatch_raw_method(pDispatchParagraphs,wdDISPID_PARAGRAPHS_ITEM,DISPATCH_METHOD,VT_DISPATCH,&pDispatchParagraph,L"\x0003",1)==S_OK&&pDispatchParagraph\
						&&_com_dispatch_raw_propget(pDispatchParagraph,wdDISPID_PARAGRAPH_RANGE,VT_DISPATCH,&pDispatchParagraphRange)==S_OK&&pDispatchParagraphRange\
						&&_com_dispatch_raw_propget(pDispatchParagraphRange,wdDISPID_RANGE_START,VT_I4,&iVal)==S_OK&&iVal==startOffset\
					) {
						wstring tempText;
						for(int i=0;listString[i]!=L'\0';++i) {
							appendCharToXML(listString[i],tempText,true);
						}
						formatAttribsStream<<L"line-prefix=\""<<tempText<<L"\" ";
					}
				}
				SysFreeString(listString);
			}
		}
	}
	if(formatConfig&formatConfig_reportRevisions) {
		long revisionType=getRevisionType(pDispatchRange);
		formatAttribsStream<<L"wdRevisionType=\""<<revisionType<<L"\" ";
	}
	if(formatConfig&formatConfig_reportStyle) {
		IDispatchPtr pDispatchStyle=NULL;
		if(_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_STYLE,VT_DISPATCH,&pDispatchStyle)==S_OK&&pDispatchStyle) {
			BSTR nameLocal=NULL;
			_com_dispatch_raw_propget(pDispatchStyle,wdDISPID_STYLE_NAMELOCAL,VT_BSTR,&nameLocal);
			if(nameLocal) {
				formatAttribsStream<<L"style=\""<<nameLocal<<L"\" ";
				SysFreeString(nameLocal);
			}
		}
	}
	if(formatConfig&formatConfig_fontFlags) {
		IDispatchPtr pDispatchFont=NULL;
		if(_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_FONT,VT_DISPATCH,&pDispatchFont)==S_OK&&pDispatchFont) {
			BSTR fontName=NULL;
			if((formatConfig&formatConfig_reportFontName)&&(_com_dispatch_raw_propget(pDispatchFont,wdDISPID_FONT_NAME,VT_BSTR,&fontName)==S_OK)&&fontName) {
				formatAttribsStream<<L"font-name=\""<<fontName<<L"\" ";
				SysFreeString(fontName);
			}
			float fVal=0.0;
			if((formatConfig&formatConfig_reportFontSize)&&(_com_dispatch_raw_propget(pDispatchFont,wdDISPID_FONT_SIZE,VT_R4,&fVal)==S_OK)) {
				formatAttribsStream<<L"font-size=\""<<fVal<<L"pt\" ";
			}
			if((formatConfig&formatConfig_reportColor)&&(_com_dispatch_raw_propget(pDispatchFont,wdDISPID_FONT_COLOR,VT_I4,&iVal)==S_OK)) {
				formatAttribsStream<<L"color=\""<<iVal<<L"\" ";
			}
			if(formatConfig&formatConfig_reportFontAttributes) {
				if(_com_dispatch_raw_propget(pDispatchFont,wdDISPID_FONT_BOLD,VT_I4,&iVal)==S_OK&&iVal) {
					formatAttribsStream<<L"bold=\"1\" ";
				}
				if(_com_dispatch_raw_propget(pDispatchFont,wdDISPID_FONT_ITALIC,VT_I4,&iVal)==S_OK&&iVal) {
					formatAttribsStream<<L"italic=\"1\" ";
				}
				if(_com_dispatch_raw_propget(pDispatchFont,wdDISPID_FONT_UNDERLINE,VT_I4,&iVal)==S_OK&&iVal) {
					formatAttribsStream<<L"underline=\"1\" ";
				}
				if(_com_dispatch_raw_propget(pDispatchFont,wdDISPID_FONT_SUPERSCRIPT,VT_I4,&iVal)==S_OK&&iVal) {
					formatAttribsStream<<L"text-position=\"super\" ";
				} else if(_com_dispatch_raw_propget(pDispatchFont,wdDISPID_FONT_SUBSCRIPT,VT_I4,&iVal)==S_OK&&iVal) {
					formatAttribsStream<<L"text-position=\"sub\" ";
				}
				if(_com_dispatch_raw_propget(pDispatchFont,wdDISPID_FONT_STRIKETHROUGH,VT_I4,&iVal)==S_OK&&iVal) {
					formatAttribsStream<<L"strikethrough=\"1\" ";
				} else if(_com_dispatch_raw_propget(pDispatchFont,wdDISPID_FONT_DOUBLESTRIKETHROUGH,VT_I4,&iVal)==S_OK&&iVal) {
					formatAttribsStream<<L"strikethrough=\"double\" ";
				}
			}
		}
	} 
	if (formatConfig&formatConfig_reportLanguage) {
		int languageId = 0;
		if (_com_dispatch_raw_propget(pDispatchRange,	wdDISPID_RANGE_LANGUAGEID, VT_I4, &languageId)==S_OK) {
			if (languageId != wdLanguageNone && languageId != wdNoProofing && languageId != wdLanguageUnknown) {
				formatAttribsStream<<L"wdLanguageId=\""<<languageId<<L"\" ";
			}
		}
	}
}

inline int getInlineShapesCount(IDispatch* pDispatchRange) {
	IDispatchPtr pDispatchShapes=NULL;
	int count=0;
	if(_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_INLINESHAPES,VT_DISPATCH,&pDispatchShapes)!=S_OK||!pDispatchShapes) {
		return 0;
	}
	if(_com_dispatch_raw_propget(pDispatchShapes,wdDISPID_INLINESHAPES_COUNT,VT_I4,&count)!=S_OK||count<=0) {
		return 0;
	} 
	return count;
}

/**
 * Generates an opening tag for the first inline shape  in this range if one exists.
  * If the function is successful, the total number of inline shapes for this range is returned allowing the caller to then perhaps move the range forward a character and try again.
   */
inline int generateInlineShapeXML(IDispatch* pDispatchRange, int offset, wostringstream& XMLStream) {
	IDispatchPtr pDispatchShapes=NULL;
	IDispatchPtr pDispatchShape=NULL;
	int count=0;
	int shapeType=0;
	BSTR altText=NULL;
	if(_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_INLINESHAPES,VT_DISPATCH,&pDispatchShapes)!=S_OK||!pDispatchShapes) {
		return 0;
	}
	if(_com_dispatch_raw_propget(pDispatchShapes,wdDISPID_INLINESHAPES_COUNT,VT_I4,&count)!=S_OK||count<=0) {
		return 0;
	}
	if(_com_dispatch_raw_method(pDispatchShapes,wdDISPID_INLINESHAPES_ITEM,DISPATCH_METHOD,VT_DISPATCH,&pDispatchShape,L"\x0003",1)!=S_OK||!pDispatchShape) {
		return 0;
	}
	if(_com_dispatch_raw_propget(pDispatchShape,wdDISPID_INLINESHAPE_TYPE,VT_I4,&shapeType)!=S_OK) {
		return 0;
	}
	wstring altTextStr=L"";
	if(_com_dispatch_raw_propget(pDispatchShape,wdDISPID_INLINESHAPE_ALTERNATIVETEXT,VT_BSTR,&altText)==S_OK&&altText) {
		for(int i=0;altText[i]!='\0';++i) {
			appendCharToXML(altText[i],altTextStr,true);
		}
		SysFreeString(altText);
	}
	altText=NULL;
	if(altTextStr.empty()&&_com_dispatch_raw_propget(pDispatchShape,wdDISPID_INLINESHAPE_TITLE,VT_BSTR,&altText)==S_OK&&altText) {
		for(int i=0;altText[i]!='\0';++i) {
			appendCharToXML(altText[i],altTextStr,true);
		}
		SysFreeString(altText);
	}
	XMLStream<<L"<control _startOfNode=\"1\" role=\""<<((shapeType==wdInlineShapePicture||shapeType==wdInlineShapeLinkedPicture)?L"graphic":L"object")<<L"\" value=\""<<altTextStr<<L"\"";
	if(shapeType==wdInlineShapeEmbeddedOLEObject) {
		XMLStream<<L" shapeoffset=\""<<offset<<L"\"";
		IDispatchPtr pOLEFormat=NULL;
		if(_com_dispatch_raw_propget(pDispatchShape,wdDISPID_INLINESHAPE_OLEFORMAT,VT_DISPATCH,&pOLEFormat)==S_OK) {
			BSTR progId=NULL;
			if(_com_dispatch_raw_propget(pOLEFormat,wdDISPID_OLEFORMAT_PROGID,VT_BSTR,&progId)==S_OK&&progId) {
				XMLStream<<L" progid=\""<<progId<<"\"";
				SysFreeString(progId);
			}
		}
	}
	XMLStream<<L">";
	return count;
}

inline bool generateFootnoteEndnoteXML(IDispatch* pDispatchRange, wostringstream& XMLStream, bool footnote) {
	IDispatchPtr pDispatchNotes=NULL;
	IDispatchPtr pDispatchNote=NULL;
	int count=0;
	int index=0;
	if(_com_dispatch_raw_propget(pDispatchRange,(footnote?wdDISPID_RANGE_FOOTNOTES:wdDISPID_RANGE_ENDNOTES),VT_DISPATCH,&pDispatchNotes)!=S_OK||!pDispatchNotes) {
		return false;
	}
	if(_com_dispatch_raw_propget(pDispatchNotes,wdDISPID_FOOTNOTES_COUNT,VT_I4,&count)!=S_OK||count<=0) {
		return false;
	}
	if(_com_dispatch_raw_method(pDispatchNotes,wdDISPID_FOOTNOTES_ITEM,DISPATCH_METHOD,VT_DISPATCH,&pDispatchNote,L"\x0003",1)!=S_OK||!pDispatchNote) {
		return false;
	}
	if(_com_dispatch_raw_propget(pDispatchNote,wdDISPID_FOOTNOTE_INDEX,VT_I4,&index)!=S_OK) {
		return false;
	}
	XMLStream<<L"<control _startOfNode=\"1\" role=\""<<(footnote?L"footnote":L"endnote")<<L"\" value=\""<<index<<L"\">";
	return true;
}

std::experimental::optional<int> getPageBreakType(IDispatchPtr pDispatchRange ) {
	// The following case should handle where we have the page break character ('0x0c') shown with '|p|'
	//	first section|p|
	//	second section.
	// range.Sections[1].pageSetup.SectionStart tells you how the section started, so we need to know
	// the next sections start type to report what kind of break this is. To do this we need to expand
	// the range, get the section start type, remove the page break character, and insert an attribute
	// for the break type.
	IDispatchPtr pDispatchRangeDup = nullptr;
	auto res = _com_dispatch_raw_propget( pDispatchRange, wdDISPID_RANGE_DUPLICATE, VT_DISPATCH, &pDispatchRangeDup);
	if( res != S_OK || !pDispatchRangeDup ) {
		LOG_DEBUGWARNING(L"error duplicating the range.");
		return {};
	}

	// we assume that we are 1 character away from the next section, this should be the value for PAGE_BREAK_VALUE ("0x0c")
	const int unitsToMove = 1;
	int unitsMoved=-1;
	res = _com_dispatch_raw_method(pDispatchRangeDup,wdDISPID_RANGE_MOVEEND,DISPATCH_METHOD,VT_I4,&unitsMoved,L"\x0003\x0003",wdCharacter,unitsToMove);
	if( res !=S_OK || unitsMoved<=0 || !pDispatchRangeDup) {
		LOG_DEBUGWARNING(L"error moving the end of the range");
		return {};
	}

	IDispatchPtr pDispatchSections = nullptr;
	res = _com_dispatch_raw_propget( pDispatchRangeDup, wdDISPID_RANGE_SECTIONS, VT_DISPATCH, &pDispatchSections);
	if( res != S_OK || !pDispatchSections ) {
		LOG_DEBUGWARNING(L"error getting sections from range");
		return {};
	}

	int count = -1;
	res = _com_dispatch_raw_propget( pDispatchSections, wdDISPID_SECTIONS_COUNT, VT_I4, &count);
	if( res != S_OK || count != 2 ) { 
		LOG_DEBUGWARNING(L"error getting section count. There should be exactly 2 sections, count: " << count);
		return {};
	}

	// we make the assumption that the second section will always be the one we want. We also assume that the section
	// count was 1 before expanding the range.
	const int sectionToGet = 2;
	IDispatchPtr pDispatchItem = nullptr;
	res = _com_dispatch_raw_method( pDispatchSections, wdDISPID_SECTIONS_ITEM, DISPATCH_METHOD, VT_DISPATCH, &pDispatchItem, L"\x0003", sectionToGet);
	if( res != S_OK || !pDispatchItem){
		LOG_DEBUGWARNING(L"error getting section item");
		return {};
	}

	IDispatchPtr pDispatchPageSetup = nullptr;
	res = _com_dispatch_raw_propget( pDispatchItem, wdDISPID_SECTION_PAGESETUP,  VT_DISPATCH, &pDispatchPageSetup);
	if( res != S_OK || !pDispatchPageSetup){
		LOG_DEBUGWARNING(L"error getting pageSetup");
		return {};
	}

	int type = -1;
	res = _com_dispatch_raw_propget( pDispatchPageSetup, wdDISPID_PAGESETUP_SECTIONSTART, VT_I4, &type);
	if( res != S_OK || type < 0){
		LOG_DEBUGWARNING(L"error getting section start");
		return {};
	}

	LOG_DEBUGWARNING(L"Got Type: " << type);
	return type;
}

UINT wm_winword_getTextInRange=0;
typedef struct {
	int startOffset;
	int endOffset;
	long formatConfig;
	BSTR text;
} winword_getTextInRange_args;
void winword_getTextInRange_helper(HWND hwnd, winword_getTextInRange_args* args) {
	//Fetch all needed objects
	//Get the window object
	IDispatchPtr pDispatchWindow=NULL;
	if(AccessibleObjectFromWindow(hwnd,OBJID_NATIVEOM,IID_IDispatch,(void**)&pDispatchWindow)!=S_OK) {
		LOG_DEBUGWARNING(L"AccessibleObjectFromWindow failed");
		return;
	}
		//Get the current selection
		IDispatchPtr pDispatchSelection=NULL;
	if(_com_dispatch_raw_propget(pDispatchWindow,wdDISPID_WINDOW_SELECTION,VT_DISPATCH,&pDispatchSelection)!=S_OK||!pDispatchSelection) {
		LOG_DEBUGWARNING(L"application.selection failed");
		return;
	}
	//Make a copy of the selection as an independent range
	IDispatchPtr pDispatchRange=NULL;
	if(_com_dispatch_raw_propget(pDispatchSelection,wdDISPID_SELECTION_RANGE,VT_DISPATCH,&pDispatchRange)!=S_OK||!pDispatchRange) {
		LOG_DEBUGWARNING(L"selection.range failed");
		return;
	}
	//Move the range to the requested offsets
	_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_SETRANGE,DISPATCH_METHOD,VT_EMPTY,NULL,L"\x0003\x0003",args->startOffset,args->endOffset);
	//A temporary stringstream for initial formatting
	wostringstream initialFormatAttribsStream;
	//Start writing the output xml to a stringstream
	wostringstream XMLStream;
	int neededClosingControlTagCount=0;
	int storyType=0;
	_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_STORYTYPE,VT_I4,&storyType);
	XMLStream<<L"<control wdStoryType=\""<<storyType<<L"\">";
	neededClosingControlTagCount+=1;
	//Collapse the range
	int initialFormatConfig=(args->formatConfig)&formatConfig_initialFormatFlags;
	int formatConfig=(args->formatConfig)&(~formatConfig_initialFormatFlags);

	IDispatchPtr paragraphRange = CreateExpandedDuplicate(pDispatchRange, wdParagraph);
	WinWord::Fields currentFields(paragraphRange);

	if((formatConfig&formatConfig_reportLinks) && false == currentFields.hasLinks() ) {
		formatConfig&=~formatConfig_reportLinks;
	}

	if((formatConfig&formatConfig_reportComments)&&(storyType==wdCommentsStory)) {
		formatConfig&=~formatConfig_reportComments;
	}
	//Check for any inline shapes in the entire range to work out whether its worth checking for them by word
	bool hasInlineShapes=(getInlineShapesCount(pDispatchRange)>0);
	vector<pair<long,long> > errorVector;
	if(formatConfig&formatConfig_reportSpellingErrors) {
		collectSpellingErrorOffsets(pDispatchRange,errorVector);
	}
	_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_COLLAPSE,DISPATCH_METHOD,VT_EMPTY,NULL,L"\x0003",wdCollapseStart);
	int chunkStartOffset=args->startOffset;
	int chunkEndOffset=chunkStartOffset;
	int unitsMoved=0;
	BSTR text=NULL;
	if(initialFormatConfig&formatConfig_reportTables) {
		neededClosingControlTagCount+=generateTableXML(pDispatchRange,(initialFormatConfig&formatConfig_includeLayoutTables)!=0,args->startOffset,args->endOffset,XMLStream);
	}
		IDispatchPtr pDispatchParagraphs=NULL;
	IDispatchPtr pDispatchParagraph=NULL;
	IDispatchPtr pDispatchParagraphRange=NULL;
	if(formatConfig&formatConfig_reportComments||initialFormatConfig&formatConfig_reportHeadings) {
		if(_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_PARAGRAPHS,VT_DISPATCH,&pDispatchParagraphs)==S_OK&&pDispatchParagraphs) {
			if(_com_dispatch_raw_method(pDispatchParagraphs,wdDISPID_PARAGRAPHS_ITEM,DISPATCH_METHOD,VT_DISPATCH,&pDispatchParagraph,L"\x0003",1)==S_OK&&pDispatchParagraph) {
				_com_dispatch_raw_propget(pDispatchParagraph,wdDISPID_PARAGRAPH_RANGE,VT_DISPATCH,&pDispatchParagraphRange);
			}
		}
	}
	vector<pair<long,long> > commentVector;
	if(formatConfig&formatConfig_reportComments) {
		collectCommentOffsets(pDispatchParagraphRange,commentVector);
	}
	if(initialFormatConfig&formatConfig_reportHeadings) {
		neededClosingControlTagCount+=generateHeadingXML(pDispatchParagraph,pDispatchParagraphRange,args->startOffset,args->endOffset,XMLStream);
	}
	generateXMLAttribsForFormatting(pDispatchRange,chunkStartOffset,chunkEndOffset,initialFormatConfig,initialFormatAttribsStream);
	{	//scope for shouldReportLinks
		const auto shouldReportLinks = (initialFormatConfig&formatConfig_reportLinks);
		if( shouldReportLinks && currentFields.hasLinks(chunkStartOffset, chunkEndOffset) ) {
			initialFormatAttribsStream<<L"link=\"1\" ";
		}
	}

	const auto shouldReportSections = (initialFormatConfig&formatConfig_reportPage);
	if(shouldReportSections) {
		int sectionNumber = -1;
		auto res = _com_dispatch_raw_method( pDispatchRange, wdDISPID_RANGE_INFORMATION, DISPATCH_PROPERTYGET, VT_I4, &sectionNumber, L"\x0003", wdActiveEndSectionNumber);
		if( S_OK == res && sectionNumber >= 0) {
			initialFormatAttribsStream << L"section-number=\""<<sectionNumber << "\" ";
		}
		else
		{
			LOG_DEBUGWARNING("Error getting the current section number. Res: "<< res <<" SectionNumber: " << sectionNumber);
		}
	}

	if(true) {
		long left = -1;
		long top = -1;
		long width = -1;
		long height = -1;
		const int wdDISPID_WINDOW_GETPOINT = 112;
		auto res = _com_dispatch_raw_method( pDispatchWindow, wdDISPID_WINDOW_GETPOINT, DISPATCH_METHOD, VT_EMPTY, nullptr,
			L"\x4003\x4003\x4003\x4003\x0009", &left, &top, &width, &height, pDispatchRange);
		if( S_OK != res) {
			LOG_DEBUGWARNING("Error getting range point from window. res: "<< res);
		}

		IDispatchPtr pDispatchApplication = nullptr;
		if( S_OK == res ) {
			res = _com_dispatch_raw_propget( pDispatchRange, wdDISPID_RANGE_APPLICATION, VT_DISPATCH, &pDispatchApplication);
			if( res != S_OK || !pDispatchApplication){
				LOG_DEBUGWARNING(L"error getting application. res: "<< res);
				res = E_FAIL;
			}
		}

		float rangePos = -1;
		if( S_OK == res ) {
			POINT topLeft;
			topLeft.x = left;
			topLeft.y = top;

			LOG_DEBUGWARNING(L"GetPoint left: " << left);
			// to handle RIGHT TO LEFT, this might need to be done for the right of the range position:
			auto ret = MapWindowPoints(HWND_DESKTOP, hwnd, &topLeft, 1);
			if (ret == 0){
				LOG_DEBUGWARNING(L"Probable error during MapWindowPoints, call SetLastError to check.");
				res = E_FAIL;
			} else {
				LOG_DEBUGWARNING(L"MapWindowPoints topLeft.x: " << topLeft.x);

				res = _com_dispatch_raw_method( pDispatchApplication, wdDISPID_APPLICATION_PIXELSTOPOINTS,
					DISPATCH_METHOD, VT_R4, &rangePos, L"\x0003", topLeft.x);
				if( res != S_OK ){
					LOG_DEBUGWARNING(L"error converting pixels to points. res: "<< res);
					res = E_FAIL;
				}
				LOG_DEBUGWARNING(L"rangePos: " << rangePos);
			}
		}

		IDispatchPtr pDispatchPageSetup = nullptr;
		if( S_OK == res ) {
			res = _com_dispatch_raw_propget( pDispatchRange, wdDISPID_RANGE_PAGESETUP, VT_DISPATCH, &pDispatchPageSetup);
			if( res != S_OK || !pDispatchPageSetup){
				LOG_DEBUGWARNING(L"error getting pageSetup. res: "<< res);
				res = E_FAIL;
			}
		}

		float pageWidth = -1.0f;
		if( S_OK == res ) {
			// page width necessary for calculating right to left positions.
			res = _com_dispatch_raw_propget( pDispatchPageSetup, wdDISPID_PAGESETUP_PAGEWIDTH, VT_R4, &pageWidth);
			if( res != S_OK || pageWidth <0){
				LOG_DEBUGWARNING(L"error getting pageWidth. res: "<< res << " pageWidth: " << pageWidth);
				res = E_FAIL;
			}
		}

		float leftMargin = -1.0f;
		if( S_OK == res ) {
			res = _com_dispatch_raw_propget( pDispatchPageSetup, wdDISPID_PAGESETUP_LEFTMARGIN, VT_R4, &leftMargin);
			if( res != S_OK || leftMargin <0){
				LOG_DEBUGWARNING(L"error getting leftMargin. res: "<< res << " leftMargin: " << leftMargin);
				res = E_FAIL;
			}
		}

		// ALSO NEED TO CONSIDER GUTTER.

		float rightMargin = -1.0f;
		if( S_OK == res ) {
			// right margin necessary for calculating right to left positions.
			res = _com_dispatch_raw_propget( pDispatchPageSetup, wdDISPID_PAGESETUP_RIGHTMARGIN, VT_R4, &rightMargin);
			if( res != S_OK || rightMargin <0){
				LOG_DEBUGWARNING(L"error getting rightMargin. res: "<< res << " rightMargin: " << rightMargin);
				res = E_FAIL;
			}
		}

		IDispatchPtr pDispatchTextColumns = nullptr;
		if( S_OK == res ) {
			res = _com_dispatch_raw_propget( pDispatchPageSetup, wdDISPID_PAGESETUP_TEXTCOLUMNS, VT_DISPATCH, &pDispatchTextColumns);
			if( res != S_OK || !pDispatchTextColumns){
				LOG_DEBUGWARNING(L"error getting textColumns. res: "<< res);
				res = E_FAIL;
			}
		}

		int count = -1;
		if( S_OK == res ) {
			res = _com_dispatch_raw_propget( pDispatchTextColumns, wdDISPID_TEXTCOLUMNS_COUNT, VT_I4, &count);
			if( res != S_OK || count < 0){
				LOG_DEBUGWARNING(L"error getting textColumn count. res: "<< res << " count: " << count);
				res = E_FAIL;
			}
		}

		if( S_OK == res ) {
			float colStartPos = leftMargin;
			// assumption: the textcolumn furthest right is last in the collection
			const int lastItemNumber = count;
			for(int itemNumber = 1; itemNumber <= lastItemNumber && S_OK == res; ++itemNumber){
				if (colStartPos <= rangePos){
					LOG_DEBUGWARNING(L"Range start is past column number: " << itemNumber);
				}
				IDispatchPtr pDispatchTextColumnItem = nullptr;
				res = _com_dispatch_raw_method( pDispatchTextColumns, wdDISPID_TEXTCOLUMNS_ITEM,
					DISPATCH_METHOD, VT_DISPATCH, &pDispatchTextColumnItem, L"\x0003", itemNumber);
				if( res != S_OK || !pDispatchTextColumnItem){
					LOG_DEBUGWARNING(L"error getting textColumn item number: "<< itemNumber << " res: "<< res);
					res = E_FAIL;
				}

				float columnWidth = -1.0f;
				if( S_OK == res ) {
					res = _com_dispatch_raw_propget( pDispatchTextColumnItem, wdDISPID_TEXTCOLUMN_WIDTH, VT_R4, &columnWidth);
					if( res != S_OK || columnWidth < 0){
						LOG_DEBUGWARNING(L"error getting textColumn width for item number: "<< itemNumber << " res: "<< res << " columnWidth: " << columnWidth);
						res = E_FAIL;
					} else {
						colStartPos += columnWidth;
						LOG_DEBUGWARNING(L"ItemNumber: " << itemNumber << " rangePos: " << rangePos << " columnWidth: "<< columnWidth << " colStartPos: " << colStartPos);
					}
				}

				float spaceAfterColumn = -1.0f;
				if( S_OK == res && itemNumber < lastItemNumber ) { // the spaceAfter property is only valid between columns
					res = _com_dispatch_raw_propget( pDispatchTextColumnItem, wdDISPID_TEXTCOLUMN_SPACEAFTER, VT_R4, &spaceAfterColumn);
					if( res != S_OK || spaceAfterColumn < 0){
						LOG_DEBUGWARNING(L"error getting textColumn spaceAfterColumn for item number: "<< itemNumber << " res: "<< res << " spaceAfterColumn: " << columnWidth);
						res = E_FAIL;
					} else {
						colStartPos += spaceAfterColumn;
						LOG_DEBUGWARNING(L"ItemNumber: " << itemNumber << " rangePos: " << rangePos << " spaceAfterColumn: "<< spaceAfterColumn << " colStartPos: " << colStartPos);
					}
				}
			}
		}
	}

	bool firstLoop=true;
	//Walk the range from the given start to end by characterFormatting or word units
	//And grab any text and formatting and generate appropriate xml
	do {
		int curDisabledFormatConfig=0;
		//generated form field xml if in a form field
		//Also automatically extends the range and chunkEndOffset to the end of the field
		const bool isFormField = TRUE == generateFormFieldXML(pDispatchRange,paragraphRange,XMLStream,chunkEndOffset);
		if(!isFormField) {
			//Move the end by word
			if(_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_MOVEEND,DISPATCH_METHOD,VT_I4,&unitsMoved,L"\x0003\x0003",wdWord,1)!=S_OK||unitsMoved<=0) {
				break;
			}
			_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_END,VT_I4,&chunkEndOffset);
		}
		const auto pageNumFieldEndIndexOptional = currentFields.getEndOfPageNumberFieldAtIndex(chunkEndOffset);
		if(pageNumFieldEndIndexOptional){
			chunkEndOffset = *pageNumFieldEndIndexOptional;
			_com_dispatch_raw_propput(pDispatchRange,wdDISPID_RANGE_END,VT_I4,chunkEndOffset);
		}

		//Make sure  that the end is not past the requested end after the move
		if(chunkEndOffset>(args->endOffset)) {
			_com_dispatch_raw_propput(pDispatchRange,wdDISPID_RANGE_END,VT_I4,args->endOffset);
			chunkEndOffset=args->endOffset;
		}
		//When using IME, the last moveEnd succeeds but the end does not really move
		if(chunkEndOffset<=chunkStartOffset) {
			LOG_DEBUGWARNING(L"moveEnd successfull but range did not expand! chunkStartOffset "<<chunkStartOffset<<L", chunkEndOffset "<<chunkEndOffset);
			break;
		}
		_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_TEXT,VT_BSTR,&text);
		if(!text) SysAllocString(L"");
		if(text) {
			int noteCharOffset=-1;
			bool isNoteChar=false;
			std::experimental::optional<int> pageBreakCharIndex;
			std::experimental::optional<int> columnBreakCharIndex;
			if(!isFormField) {
				//Force a new chunk before and after control+b (note characters)
				for(int i=0;text[i]!=L'\0';++i) {
					if(text[i]==L'\x0002') {
						noteCharOffset=i;
						if(i==0) text[i]=L' ';
						break;
					}  else if(text[i]==L'\x0007'&&(chunkEndOffset-chunkStartOffset)==1) {
						text[i]=L'\0';
						//Collecting revision info does not work on cell delimiters
						curDisabledFormatConfig|=formatConfig_reportRevisions;
					} else if( text[i] == PAGE_BREAK_VALUE) { // page break
						pageBreakCharIndex = i;
					} else if( text[i] == COLUMN_BREAK_VALUE) { // column break
						columnBreakCharIndex = i;
					}
				}
				isNoteChar=(noteCharOffset==0);
				if(noteCharOffset==0) noteCharOffset=1;
				if(noteCharOffset>0) {
					text[noteCharOffset]=L'\0';
					_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_COLLAPSE,DISPATCH_METHOD,VT_EMPTY,NULL,L"\x0003",wdCollapseStart);
					if(_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_MOVEEND,DISPATCH_METHOD,VT_I4,&unitsMoved,L"\x0003\x0003",wdCharacter,noteCharOffset)!=S_OK||unitsMoved<=0) {
						break;
					}
					_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_END,VT_I4,&chunkEndOffset);
				}
			}
			if(isNoteChar) {
				isNoteChar=generateFootnoteEndnoteXML(pDispatchRange,XMLStream,true);
				if(!isNoteChar) isNoteChar=generateFootnoteEndnoteXML(pDispatchRange,XMLStream,false);
			}
			//If there are inline shapes somewhere, try getting and generating info for the first one hear.
			//We also get the over all count of shapes for this word so we know whether we need to check for more within this word
			int inlineShapesCount=hasInlineShapes?generateInlineShapeXML(pDispatchRange,chunkStartOffset,XMLStream):0;
			if(inlineShapesCount>1) {
				_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_COLLAPSE,DISPATCH_METHOD,VT_EMPTY,NULL,L"\x0003",wdCollapseStart);
				if(_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_MOVEEND,DISPATCH_METHOD,VT_I4,&unitsMoved,L"\x0003\x0003",wdCharacter,1)!=S_OK||unitsMoved<=0) {
					break;
				}
				_com_dispatch_raw_propget(pDispatchRange,wdDISPID_RANGE_END,VT_I4,&chunkEndOffset);
			}
			XMLStream<<L"<text _startOffset=\""<<chunkStartOffset<<L"\" _endOffset=\""<<chunkEndOffset<<L"\" ";
			XMLStream<<initialFormatAttribsStream.str();

			{	// scope for xmlAttribsFormatConfig
				const auto xmlAttribsFormatConfig = formatConfig&(~curDisabledFormatConfig);

				if( pageBreakCharIndex ){
					auto type = getPageBreakType(pDispatchRange);
					if(type){
						text[*pageBreakCharIndex] = '\0';
						XMLStream << L"section-break=\"" << *type << "\" ";
					}
				}
				if (columnBreakCharIndex){
					text[*columnBreakCharIndex] = '\0';
					XMLStream << L"column-break=\"" << 1 << "\" ";
				}

				generateXMLAttribsForFormatting(pDispatchRange,chunkStartOffset,chunkEndOffset,xmlAttribsFormatConfig,XMLStream);
				const auto shouldReportLinks = (xmlAttribsFormatConfig&formatConfig_reportLinks);
				if( shouldReportLinks && currentFields.hasLinks(chunkStartOffset, chunkEndOffset) ) {
					XMLStream<<L"link=\"1\" ";
				}
			}

			for(vector<pair<long,long>>::iterator i=errorVector.begin();i!=errorVector.end();++i) {
				if(chunkStartOffset>=i->first&&chunkStartOffset<i->second) {
					XMLStream<<L" invalid-spelling=\"1\" ";
					break;
				}
			}
			for(vector<pair<long,long>>::iterator i=commentVector.begin();i!=commentVector.end();++i) {
				if(!(chunkStartOffset>=i->second||chunkEndOffset<=i->first)) {
					XMLStream<<L" comment=\""<<(i->second)<<L"\" ";
					break;
				}
			}
			XMLStream<<L">";
			if(firstLoop) {
				formatConfig&=(~formatConfig_reportLists);
				firstLoop=false;
			}
			wstring tempText;
			if(inlineShapesCount>0) {
				tempText+=L" ";
			} else for(int i=0;text[i]!=L'\0';++i) {
				appendCharToXML(text[i],tempText);
			}
			XMLStream<<tempText;
			SysFreeString(text);
			text=NULL;
			XMLStream<<L"</text>";
			if(isFormField) XMLStream<<L"</control>";
			if(isNoteChar) XMLStream<<L"</control>";
			if(inlineShapesCount>0) XMLStream<<L"</control>";
		}
		_com_dispatch_raw_method(pDispatchRange,wdDISPID_RANGE_COLLAPSE,DISPATCH_METHOD,VT_EMPTY,NULL,L"\x0003",wdCollapseEnd);
		chunkStartOffset=chunkEndOffset;
	} while(chunkEndOffset<(args->endOffset));
	for(;neededClosingControlTagCount>0;--neededClosingControlTagCount) {
		XMLStream<<L"</control>";
	}
	args->text=SysAllocString(XMLStream.str().c_str());
}

UINT wm_winword_moveByLine=0;
typedef struct {
	int offset;
	int moveBack;
	int newOffset;
} winword_moveByLine_args;
void winword_moveByLine_helper(HWND hwnd, winword_moveByLine_args* args) {
	//Fetch all needed objects
	IDispatchPtr pDispatchWindow=NULL;
	if(AccessibleObjectFromWindow(hwnd,OBJID_NATIVEOM,IID_IDispatch,(void**)&pDispatchWindow)!=S_OK||!pDispatchWindow) {
		LOG_DEBUGWARNING(L"AccessibleObjectFromWindow failed");
		return;
	}
	IDispatchPtr pDispatchApplication=NULL;
	if(_com_dispatch_raw_propget(pDispatchWindow,wdDISPID_WINDOW_APPLICATION,VT_DISPATCH,&pDispatchApplication)!=S_OK||!pDispatchApplication) {
		LOG_DEBUGWARNING(L"window.application failed");
		return;
	}
	IDispatchPtr pDispatchSelection=NULL;
	if(_com_dispatch_raw_propget(pDispatchWindow,wdDISPID_WINDOW_SELECTION,VT_DISPATCH,&pDispatchSelection)!=S_OK||!pDispatchSelection) {
		LOG_DEBUGWARNING(L"application.selection failed");
		return;
	}
	BOOL startWasActive=false;
	if(_com_dispatch_raw_propget(pDispatchSelection,wdDISPID_SELECTION_STARTISACTIVE,VT_BOOL,&startWasActive)!=S_OK) {
		LOG_DEBUGWARNING(L"selection.StartIsActive failed");
	}
	IDispatch* pDispatchOldSelRange=NULL;
	if(_com_dispatch_raw_propget(pDispatchSelection,wdDISPID_SELECTION_RANGE,VT_DISPATCH,&pDispatchOldSelRange)!=S_OK||!pDispatchOldSelRange) {
		LOG_DEBUGWARNING(L"selection.range failed");
		return;
	}
	//Disable screen updating as we will be moving the selection temporarily
	_com_dispatch_raw_propput(pDispatchApplication,wdDISPID_APPLICATION_SCREENUPDATING,VT_BOOL,false);
	//Move the selection to the given range
	_com_dispatch_raw_method(pDispatchSelection,wdDISPID_SELECTION_SETRANGE,DISPATCH_METHOD,VT_EMPTY,NULL,L"\x0003\x0003",args->offset,args->offset);
// Move the selection by 1 line
	int unitsMoved=0;
	_com_dispatch_raw_method(pDispatchSelection,wdDISPID_RANGE_MOVE,DISPATCH_METHOD,VT_I4,&unitsMoved,L"\x0003\x0003",wdLine,((args->moveBack)?-1:1));
	_com_dispatch_raw_propget(pDispatchSelection,wdDISPID_RANGE_START,VT_I4,&(args->newOffset));
	//Move the selection back to its original location
	_com_dispatch_raw_method(pDispatchOldSelRange,wdDISPID_RANGE_SELECT,DISPATCH_METHOD,VT_EMPTY,NULL,NULL);
	//Restore the old selection direction
	_com_dispatch_raw_propput(pDispatchSelection,wdDISPID_SELECTION_STARTISACTIVE,VT_BOOL,startWasActive);
	//Reenable screen updating
	_com_dispatch_raw_propput(pDispatchApplication,wdDISPID_APPLICATION_SCREENUPDATING,VT_BOOL,true);
}

LRESULT CALLBACK winword_callWndProcHook(int code, WPARAM wParam, LPARAM lParam) {
	CWPSTRUCT* pcwp=(CWPSTRUCT*)lParam;
	if(pcwp->message==wm_winword_expandToLine) {
		winword_expandToLine_helper(pcwp->hwnd,reinterpret_cast<winword_expandToLine_args*>(pcwp->wParam));
	} else if(pcwp->message==wm_winword_getTextInRange) {
		winword_getTextInRange_helper(pcwp->hwnd,reinterpret_cast<winword_getTextInRange_args*>(pcwp->wParam));
	} else if(pcwp->message==wm_winword_moveByLine) {
		winword_moveByLine_helper(pcwp->hwnd,reinterpret_cast<winword_moveByLine_args*>(pcwp->wParam));
	}
	return 0;
}

error_status_t nvdaInProcUtils_winword_expandToLine(handle_t bindingHandle, const unsigned long windowHandle, const int offset, int* lineStart, int* lineEnd) {
	winword_expandToLine_args args={offset,-1,-1};
	DWORD_PTR wmRes=0;
	SendMessage((HWND)UlongToHandle(windowHandle),wm_winword_expandToLine,(WPARAM)&args,0);
	*lineStart=args.lineStart;
	*lineEnd=args.lineEnd;
	return RPC_S_OK;
}

error_status_t nvdaInProcUtils_winword_getTextInRange(handle_t bindingHandle, const unsigned long windowHandle, const int startOffset, const int endOffset, const long formatConfig, BSTR* text) { 
	winword_getTextInRange_args args={startOffset,endOffset,formatConfig,NULL};
	SendMessage((HWND)UlongToHandle(windowHandle),wm_winword_getTextInRange,(WPARAM)&args,0);
	*text=args.text;
	return RPC_S_OK;
}

error_status_t nvdaInProcUtils_winword_moveByLine(handle_t bindingHandle, const unsigned long windowHandle, const int offset, const int moveBack, int* newOffset) {
	winword_moveByLine_args args={offset,moveBack,NULL};
	SendMessage((HWND)UlongToHandle(windowHandle),wm_winword_moveByLine,(WPARAM)&args,0);
	*newOffset=args.newOffset;
	return RPC_S_OK;
}

void winword_inProcess_initialize() {
	wm_winword_expandToLine=RegisterWindowMessage(L"wm_winword_expandToLine");
	wm_winword_getTextInRange=RegisterWindowMessage(L"wm_winword_getTextInRange");
	wm_winword_moveByLine=RegisterWindowMessage(L"wm_winword_moveByLine");
	registerWindowsHook(WH_CALLWNDPROC,winword_callWndProcHook);
}

void winword_inProcess_terminate() {
	unregisterWindowsHook(WH_CALLWNDPROC,winword_callWndProcHook);
}
