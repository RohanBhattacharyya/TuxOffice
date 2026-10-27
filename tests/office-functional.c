/* Automated Office object-model round trips, using fresh documents only.
 * Requires an isolated TuxOffice prefix. No account or messaging operations.
 */
#define COBJMACROS
#include <windows.h>
#include <oleauto.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

static unsigned int checks;
static ULONGLONG epoch;
static void check(BOOL condition, const char *message)
{
    ++checks;
    printf("CHECK %u %s %s\n", checks, condition ? "PASS" : "FAIL", message);
    fflush(stdout);
    if (!condition) ExitProcess(3);
}
static VARIANT integer(LONG x) { VARIANT v; VariantInit(&v); V_VT(&v)=VT_I4; V_I4(&v)=x; return v; }
static VARIANT bool_value(BOOL x) { VARIANT v; VariantInit(&v); V_VT(&v)=VT_BOOL; V_BOOL(&v)=x?VARIANT_TRUE:VARIANT_FALSE; return v; }
static VARIANT real(double x) { VARIANT v; VariantInit(&v); V_VT(&v)=VT_R8; V_R8(&v)=x; return v; }
static VARIANT string(const WCHAR *x) { VARIANT v; VariantInit(&v); V_VT(&v)=VT_BSTR; V_BSTR(&v)=SysAllocString(x); return v; }
static VARIANT object(IDispatch *x) { VARIANT v; VariantInit(&v); V_VT(&v)=VT_DISPATCH; V_DISPATCH(&v)=x; IDispatch_AddRef(x); return v; }
static VARIANT invoke(IDispatch *obj, const WCHAR *name, WORD flags, unsigned int count, ...)
{
    VARIANT args[16], result;
    DISPID id, put=DISPID_PROPERTYPUT;
    DISPPARAMS params={args,NULL,count,0};
    EXCEPINFO exception={0};
    HRESULT hr;
    va_list ap;
    unsigned int i;
    ULONGLONG started=GetTickCount64();
    printf("CALL %llu %ls\n", started-epoch,name); fflush(stdout);
    if (!obj) { fprintf(stderr,"NULL object for %ls\n",name); ExitProcess(2); }
    va_start(ap,count);
    for(i=0;i<count;++i) args[count-i-1]=va_arg(ap,VARIANT);
    va_end(ap);
    if (flags & DISPATCH_PROPERTYPUT) { params.rgdispidNamedArgs=&put; params.cNamedArgs=1; }
    hr=IDispatch_GetIDsOfNames(obj,&IID_NULL,(WCHAR **)&name,1,LOCALE_USER_DEFAULT,&id);
    if (SUCCEEDED(hr)) do {
        VariantInit(&result);
        hr=IDispatch_Invoke(obj,id,&IID_NULL,LOCALE_USER_DEFAULT,flags,&params,&result,&exception,NULL);
        if (hr != RPC_E_CALL_REJECTED && hr != RPC_E_SERVERCALL_RETRYLATER) break;
        Sleep(100);
    } while(GetTickCount64()-started<30000);
    for(i=0;i<count;++i) VariantClear(&args[i]);
    if(FAILED(hr)) {
        fprintf(stderr,"FAILED %ls hr=%08lx scode=%08lx description=%ls\n",name,hr,exception.scode,
                exception.bstrDescription?exception.bstrDescription:L"");
        ExitProcess(2);
    }
    SysFreeString(exception.bstrSource); SysFreeString(exception.bstrDescription); SysFreeString(exception.bstrHelpFile);
    printf("DONE %ls ms=%llu\n",name,GetTickCount64()-started); fflush(stdout);
    return result;
}
#define GET(o,n) dispatch_result(invoke(o,L##n,DISPATCH_PROPERTYGET,0))
#define PUT(o,n,v) clear_result(invoke(o,L##n,DISPATCH_PROPERTYPUT,1,v))
#define CALL0(o,n) clear_result(invoke(o,L##n,DISPATCH_METHOD,0))
static void clear_result(VARIANT v) { VariantClear(&v); }
static IDispatch *dispatch_result(VARIANT v)
{
    if(V_VT(&v)!=VT_DISPATCH || !V_DISPATCH(&v)) { fprintf(stderr,"Expected dispatch, got vt=%u\n",V_VT(&v)); ExitProcess(2); }
    return V_DISPATCH(&v);
}
static LONG number_result(VARIANT v)
{
    VARIANT converted; LONG n;
    VariantInit(&converted);
    if(FAILED(VariantChangeType(&converted,&v,0,VT_I4))) ExitProcess(2);
    n=V_I4(&converted); VariantClear(&converted); VariantClear(&v); return n;
}
static LONG number(IDispatch *o,const WCHAR *name) { return number_result(invoke(o,name,DISPATCH_PROPERTYGET,0)); }
static IDispatch *item(IDispatch *o,LONG n) { return dispatch_result(invoke(o,L"Item",DISPATCH_PROPERTYGET|DISPATCH_METHOD,1,integer(n))); }
static IDispatch *range(IDispatch *sheet,const WCHAR *address)
{ return dispatch_result(invoke(sheet,L"Range",DISPATCH_PROPERTYGET,1,string(address))); }
static void release(IDispatch *o) { if(o) IDispatch_Release(o); }
static void make_path(WCHAR *dest,const WCHAR *directory,const WCHAR *file)
{ swprintf(dest,1024,L"%ls\\%ls",directory,file); }
static void pump_for(DWORD milliseconds)
{
    HANDLE event=CreateEventW(NULL,TRUE,FALSE,NULL);
    DWORD index;
    CoWaitForMultipleHandles(0,milliseconds,1,&event,&index);
    CloseHandle(event);
}

static void word(IDispatch *app,const WCHAR *directory)
{
    IDispatch *documents=GET(app,"Documents"), *doc, *content, *font, *tables, *table, *cell, *text;
    IDispatch *comments, *comment, *shapes, *shape, *sections, *section, *headers, *header, *reopened;
    WCHAR path[1024],pdf[1024],body[16000];
    LONG end;
    unsigned int i;
    PUT(app,"DisplayAlerts",integer(0));
    doc=dispatch_result(invoke(documents,L"Add",DISPATCH_METHOD,0));
    body[0]=0;
    for(i=0;i<50;++i) wcscat(body,L"TuxOffice document regression: editing, typography, tables and comments.\r");
    content=GET(doc,"Content");PUT(content,"Text",string(body));
    font=GET(content,"Font");PUT(font,"Name",string(L"Calibri"));PUT(font,"Size",real(14));release(font);
    check(number(content,L"End")>3000,"Word inserted multipage document text");
    PUT(doc,"TrackRevisions",bool_value(TRUE));
    clear_result(invoke(content,L"InsertBefore",DISPATCH_METHOD,1,string(L"Tracked insertion verified.\r")));
    {
        IDispatch *revisions=GET(doc,"Revisions");
        check(number(revisions,L"Count")>0,"Word records tracked insertion");
        CALL0(revisions,"AcceptAll");
        check(number(revisions,L"Count")==0,"Word accepts tracked changes");
        release(revisions);
    }
    PUT(doc,"TrackRevisions",bool_value(FALSE));
    end=number(content,L"End")-1;release(content);
    content=dispatch_result(invoke(doc,L"Range",DISPATCH_METHOD,2,integer(end),integer(end)));
    tables=GET(doc,"Tables");table=dispatch_result(invoke(tables,L"Add",DISPATCH_METHOD,3,object(content),integer(4),integer(3)));
    release(content);
    cell=dispatch_result(invoke(table,L"Cell",DISPATCH_METHOD,2,integer(2),integer(2)));
    text=GET(cell,"Range");PUT(text,"Text",string(L"Verified table cell"));release(text);release(cell);release(table);
    check(number(tables,L"Count")==1,"Word created a table");release(tables);
    content=dispatch_result(invoke(doc,L"Range",DISPATCH_METHOD,2,integer(0),integer(9)));
    comments=GET(doc,"Comments");comment=dispatch_result(invoke(comments,L"Add",DISPATCH_METHOD,2,object(content),string(L"Automated regression comment")));
    release(comment);release(content);check(number(comments,L"Count")==1,"Word created a comment");release(comments);
    sections=GET(doc,"Sections");section=item(sections,1);headers=GET(section,"Headers");header=item(headers,1);
    text=GET(header,"Range");PUT(text,"Text",string(L"TuxOffice regression header"));
    release(text);release(header);release(headers);release(section);release(sections);
    shapes=GET(doc,"Shapes");shape=dispatch_result(invoke(shapes,L"AddShape",DISPATCH_METHOD,5,integer(1),real(20),real(20),real(80),real(40)));
    release(shape);check(number(shapes,L"Count")==1,"Word created a drawing shape");release(shapes);
    clear_result(invoke(doc,L"ComputeStatistics",DISPATCH_METHOD,1,integer(2)));
    make_path(path,directory,L"word-document.docx");make_path(pdf,directory,L"word-document.pdf");
    clear_result(invoke(doc,L"SaveAs2",DISPATCH_METHOD,2,string(path),integer(16)));
    clear_result(invoke(doc,L"ExportAsFixedFormat",DISPATCH_METHOD,2,string(pdf),integer(17)));
    clear_result(invoke(doc,L"Close",DISPATCH_METHOD,1,integer(0)));release(doc);
    reopened=dispatch_result(invoke(documents,L"Open",DISPATCH_METHOD,1,string(path)));
    text=GET(reopened,"Content");
    {
        VARIANT value=invoke(text,L"Text",DISPATCH_PROPERTYGET,0);
        check(V_VT(&value)==VT_BSTR && wcsstr(V_BSTR(&value),L"Tracked insertion verified."),
                "accepted insertion survives DOCX reopen");
        VariantClear(&value);
    }
    release(text);
    tables=GET(reopened,"Tables");check(number(tables,L"Count")==1,"table survives DOCX reopen");
    table=item(tables,1);cell=dispatch_result(invoke(table,L"Cell",DISPATCH_METHOD,2,integer(2),integer(2)));
    text=GET(cell,"Range");
    {
        VARIANT value=invoke(text,L"Text",DISPATCH_PROPERTYGET,0);
        check(V_VT(&value)==VT_BSTR && wcsstr(V_BSTR(&value),L"Verified table cell"),"table cell text survives DOCX reopen");
        VariantClear(&value);
    }
    release(text);release(cell);release(table);release(tables);
    comments=GET(reopened,"Comments");check(number(comments,L"Count")==1,"comment survives DOCX reopen");release(comments);
    shapes=GET(reopened,"Shapes");check(number(shapes,L"Count")==1,"drawing survives DOCX reopen");release(shapes);
    clear_result(invoke(reopened,L"Close",DISPATCH_METHOD,1,integer(0)));release(reopened);release(documents);
}

static void excel(IDispatch *app,const WCHAR *directory)
{
    IDispatch *books=GET(app,"Workbooks"), *book, *sheet, *cells, *formulas, *charts, *chart_object, *chart, *title;
    IDispatch *series,*one,*points,*reopened,*reopened_charts;
    SAFEARRAYBOUND bounds[2]={{101,1},{2,1}};
    SAFEARRAY *array=SafeArrayCreate(VT_VARIANT,2,bounds);
    VARIANT values, v;
    WCHAR path[1024],pdf[1024];
    LONG row,col,indices[2];
    unsigned int i;
    static const LONG types[]={-4169,4,51,-4169};
    PUT(app,"DisplayAlerts",bool_value(FALSE));
    book=dispatch_result(invoke(books,L"Add",DISPATCH_METHOD,0));
    sheet=GET(app,"ActiveSheet");
    for(row=1;row<=101;++row) for(col=1;col<=2;++col) {
        indices[0]=row;indices[1]=col;
        v=row==1?string(col==1?L"X":L"Y"):integer(col==1?row-1:(row-1)*(row-1));
        if(FAILED(SafeArrayPutElement(array,indices,&v))) ExitProcess(2); VariantClear(&v);
    }
    VariantInit(&values);V_VT(&values)=VT_ARRAY|VT_VARIANT;V_ARRAY(&values)=array;
    cells=range(sheet,L"A1:B101");PUT(cells,"Value2",values);
    formulas=range(sheet,L"C2:C101");PUT(formulas,"FormulaR1C1",string(L"=RC[-2]+RC[-1]"));
    CALL0(app,"CalculateFull");
    one=range(sheet,L"C101");check(number(one,L"Value2")==10100,"Excel formula result 10100");release(one);
    clear_result(invoke(cells,L"AutoFilter",DISPATCH_METHOD,0));
    clear_result(invoke(cells,L"AutoFilter",DISPATCH_METHOD,0));
    {
        IDispatch *data=range(sheet,L"A2:C101"),*key=range(sheet,L"A2");
        IDispatch *destination=range(sheet,L"E2:G101"),*formats,*condition;
        clear_result(invoke(data,L"Sort",DISPATCH_METHOD,2,object(key),integer(2)));
        one=range(sheet,L"A2");check(number(one,L"Value2")==100,"Excel sorts descending");release(one);
        one=range(sheet,L"C2");check(number(one,L"Value2")==10100,"formulas follow descending sort");release(one);
        clear_result(invoke(data,L"Sort",DISPATCH_METHOD,2,object(key),integer(1)));
        one=range(sheet,L"A2");check(number(one,L"Value2")==1,"Excel sorts ascending");release(one);
        clear_result(invoke(data,L"Copy",DISPATCH_METHOD,1,object(destination)));
        one=range(sheet,L"G101");check(number(one,L"Value2")==10100,"copied relative formulas calculate correctly");release(one);
        formats=GET(destination,"FormatConditions");
        condition=dispatch_result(invoke(formats,L"Add",DISPATCH_METHOD,3,integer(1),integer(5),string(L"5000")));
        check(number(formats,L"Count")==1,"Excel creates conditional formatting");
        release(condition);release(formats);release(destination);release(key);release(data);
    }
    charts=dispatch_result(invoke(sheet,L"ChartObjects",DISPATCH_METHOD|DISPATCH_PROPERTYGET,0));
    chart_object=dispatch_result(invoke(charts,L"Add",DISPATCH_METHOD,4,real(280),real(20),real(520),real(340)));
    chart=GET(chart_object,"Chart");
    for(i=0;i<sizeof(types)/sizeof(*types);++i) {
        PUT(chart,"ChartType",integer(types[i]));
        clear_result(invoke(chart,L"SetSourceData",DISPATCH_METHOD,1,object(cells)));
        PUT(chart,"ChartType",integer(types[i]));
        check(number(chart,L"ChartType")==types[i],"chart type set and read back");
        Sleep(200);
    }
    CALL0(chart_object,"Activate");
    CALL0(chart_object,"Select");
    Sleep(800);
    PUT(chart,"HasTitle",bool_value(TRUE));title=GET(chart,"ChartTitle");PUT(title,"Text",string(L"TuxOffice scatter regression"));release(title);
    series=dispatch_result(invoke(chart,L"SeriesCollection",DISPATCH_METHOD|DISPATCH_PROPERTYGET,0));
    check(number(series,L"Count")==1,"scatter has one data series");
    one=item(series,1);points=dispatch_result(invoke(one,L"Points",DISPATCH_METHOD|DISPATCH_PROPERTYGET,0));
    check(number(points,L"Count")==100,"scatter has 100 points");release(points);release(one);release(series);
    make_path(path,directory,L"excel-scatter.xlsx");make_path(pdf,directory,L"excel-scatter.pdf");
    clear_result(invoke(book,L"SaveAs",DISPATCH_METHOD,2,string(path),integer(51)));
    clear_result(invoke(book,L"ExportAsFixedFormat",DISPATCH_METHOD,2,integer(0),string(pdf)));
    release(chart);release(chart_object);release(charts);release(formulas);release(cells);release(sheet);
    clear_result(invoke(book,L"Close",DISPATCH_METHOD,1,bool_value(FALSE)));release(book);
    reopened=dispatch_result(invoke(books,L"Open",DISPATCH_METHOD,1,string(path)));
    sheet=GET(app,"ActiveSheet");one=range(sheet,L"C101");
    check(number(one,L"Value2")==10100,"formula survives XLSX reopen");release(one);
    one=range(sheet,L"G101");check(number(one,L"Value2")==10100,"copied formula survives XLSX reopen");release(one);
    one=range(sheet,L"E2:G101");series=GET(one,"FormatConditions");
    check(number(series,L"Count")==1,"conditional formatting survives XLSX reopen");release(series);release(one);
    reopened_charts=dispatch_result(invoke(sheet,L"ChartObjects",DISPATCH_METHOD|DISPATCH_PROPERTYGET,0));
    check(number(reopened_charts,L"Count")==1,"chart survives XLSX reopen");
    one=item(reopened_charts,1);chart=GET(one,"Chart");
    check(number(chart,L"ChartType")==-4169,"scatter type survives XLSX reopen");
    release(chart);release(one);release(reopened_charts);release(sheet);
    clear_result(invoke(reopened,L"Close",DISPATCH_METHOD,1,bool_value(FALSE)));release(reopened);release(books);
}

static void powerpoint(IDispatch *app,const WCHAR *directory)
{
    IDispatch *presentations=GET(app,"Presentations"),*presentation,*slides,*slide,*shapes,*shape,*frame,*text;
    IDispatch *settings,*show,*view,*reopened;
    WCHAR path[1024],pdf[1024],label[128];
    unsigned int i;
    presentation=dispatch_result(invoke(presentations,L"Add",DISPATCH_METHOD,1,integer(-1)));
    slides=GET(presentation,"Slides");
    for(i=1;i<=5;++i) {
        slide=dispatch_result(invoke(slides,L"Add",DISPATCH_METHOD,2,integer(i),integer(12)));
        shapes=GET(slide,"Shapes");
        shape=dispatch_result(invoke(shapes,L"AddTextbox",DISPATCH_METHOD,5,integer(1),real(40),real(40),real(600),real(100)));
        frame=GET(shape,"TextFrame");text=GET(frame,"TextRange");
        swprintf(label,128,L"TuxOffice slide %u: text, shapes, save and reopen",i);PUT(text,"Text",string(label));
        release(text);release(frame);release(shape);
        shape=dispatch_result(invoke(shapes,L"AddShape",DISPATCH_METHOD,5,integer(1),real(80),real(180),real(300),real(180)));
        release(shape);
        if(i==4)
        {
            IDispatch *chart,*data,*book,*sheets,*sheet,*values;
            shape=dispatch_result(invoke(shapes,L"AddChart2",DISPATCH_METHOD,6,integer(-1),integer(-4169),
                    real(400),real(180),real(280),real(200)));
            chart=GET(shape,"Chart");
            check(number(chart,L"ChartType")==-4169,"PowerPoint inserts a scatter chart");
            data=GET(chart,"ChartData");
            {
                char full[8];
                if(GetEnvironmentVariableA("TUXOFFICE_CHART_FULL_EXCEL",full,sizeof(full))) CALL0(data,"Activate");
                else CALL0(data,"ActivateChartDataWindow");
            }
            book=GET(data,"Workbook");
            sheets=GET(book,"Worksheets");sheet=item(sheets,1);values=range(sheet,L"B2");
            PUT(values,"Value2",integer(37));
            check(number(values,L"Value2")==37,"PowerPoint embedded Excel data can be edited");
            release(values);release(sheet);release(sheets);
            {
                char delay[32];
                if(GetEnvironmentVariableA("TUXOFFICE_CHART_CLOSE_DELAY_MS",delay,sizeof(delay)))
                {
                    puts("READY embedded chart close");fflush(stdout);
                    pump_for(strtoul(delay,NULL,10));
                }
            }
            {
                WCHAR trigger[1024];
                if(GetEnvironmentVariableW(L"TUXOFFICE_CHART_CLOSE_TRIGGER",trigger,sizeof(trigger)/sizeof(*trigger)))
                {
                    ULONGLONG deadline=GetTickCount64()+45000;
                    puts("READY chart editor UI test");fflush(stdout);
                    while(GetFileAttributesW(trigger)==INVALID_FILE_ATTRIBUTES && GetTickCount64()<deadline)
                        pump_for(100);
                    check(GetFileAttributesW(trigger)!=INVALID_FILE_ATTRIBUTES,"chart editor closed through its window");
                }
                else clear_result(invoke(book,L"Close",DISPATCH_METHOD,1,bool_value(TRUE)));
            }
            release(book);release(data);CALL0(chart,"Refresh");release(chart);release(shape);
        }
        if(i==5)
        {
            IDispatch *table,*cell;
            shape=dispatch_result(invoke(shapes,L"AddTable",DISPATCH_METHOD,6,integer(2),integer(2),
                    real(400),real(180),real(240),real(100)));
            table=GET(shape,"Table");cell=dispatch_result(invoke(table,L"Cell",DISPATCH_METHOD,2,integer(2),integer(2)));
            release(shape);shape=GET(cell,"Shape");frame=GET(shape,"TextFrame");text=GET(frame,"TextRange");
            PUT(text,"Text",string(L"Verified slide table cell"));
            release(text);release(frame);release(shape);release(cell);release(table);
            check(number(shapes,L"Count")==3,"PowerPoint inserts an editable table");
        }
        release(shapes);release(slide);
    }
    check(number(slides,L"Count")==5,"PowerPoint created five slides");
    settings=GET(presentation,"SlideShowSettings");show=dispatch_result(invoke(settings,L"Run",DISPATCH_METHOD,0));
    view=GET(show,"View");
    for(i=0;i<4;++i) {CALL0(view,"Next");Sleep(100);}
    CALL0(view,"Exit");release(view);release(show);release(settings);
    make_path(path,directory,L"powerpoint-slides.pptx");make_path(pdf,directory,L"powerpoint-slides.pdf");
    clear_result(invoke(presentation,L"SaveAs",DISPATCH_METHOD,2,string(path),integer(24)));
    clear_result(invoke(presentation,L"ExportAsFixedFormat",DISPATCH_METHOD,2,string(pdf),integer(2)));
    release(slides);CALL0(presentation,"Close");release(presentation);
    reopened=dispatch_result(invoke(presentations,L"Open",DISPATCH_METHOD,4,string(path),integer(0),integer(0),integer(-1)));
    slides=GET(reopened,"Slides");check(number(slides,L"Count")==5,"five slides survive PPTX reopen");
    slide=item(slides,5);shapes=GET(slide,"Shapes");check(number(shapes,L"Count")==3,"text, rectangle and table survive PPTX reopen");
    shape=item(shapes,3);frame=GET(shape,"Table");
    text=dispatch_result(invoke(frame,L"Cell",DISPATCH_METHOD,2,integer(2),integer(2)));
    release(shape);shape=GET(text,"Shape");release(text);release(frame);
    frame=GET(shape,"TextFrame");text=GET(frame,"TextRange");
    {
        VARIANT value=invoke(text,L"Text",DISPATCH_PROPERTYGET,0);
        check(V_VT(&value)==VT_BSTR && !wcscmp(V_BSTR(&value),L"Verified slide table cell"),
                "table cell text survives PPTX reopen");
        VariantClear(&value);
    }
    release(text);release(frame);release(shape);release(shapes);release(slide);
    slide=item(slides,4);shapes=GET(slide,"Shapes");shape=item(shapes,3);frame=GET(shape,"Chart");
    check(number(frame,L"ChartType")==-4169,"embedded scatter survives PPTX reopen");
    release(frame);release(shape);release(shapes);release(slide);release(slides);
    CALL0(reopened,"Close");release(reopened);release(presentations);
}

int main(int argc,char **argv)
{
    WCHAR progid[128],directory[1024];
    char prefix[4096];
    CLSID clsid;
    IDispatch *app=NULL;
    HRESULT hr;
    WCHAR *slash;
    if(argc!=3) return 1;
    if(!GetEnvironmentVariableA("TUXOFFICE_PREFIX",prefix,sizeof(prefix)) || !strstr(prefix,"/home/prefix")) {
        fputs("Functional tests require the isolated Office test prefix\n",stderr);return 4;
    }
    if(strcmp(argv[1],"word") && strcmp(argv[1],"excel") && strcmp(argv[1],"powerpoint")) return 1;
    MultiByteToWideChar(CP_UTF8,0,argv[2],-1,directory,1024);
    for(slash=directory;*slash;++slash) if(*slash==L'/') *slash=L'\\';
    swprintf(progid,128,L"%ls.Application",!strcmp(argv[1],"excel")?L"Excel":!strcmp(argv[1],"word")?L"Word":L"PowerPoint");
    epoch=GetTickCount64();CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    hr=CLSIDFromProgID(progid,&clsid);
    if(SUCCEEDED(hr)) hr=CoCreateInstance(&clsid,NULL,CLSCTX_LOCAL_SERVER,&IID_IDispatch,(void **)&app);
    if(FAILED(hr)) {fprintf(stderr,"Create %ls failed %08lx\n",progid,hr);return 2;}
    PUT(app,"Visible",bool_value(TRUE));
    if(!strcmp(argv[1],"excel")) excel(app,directory);
    else if(!strcmp(argv[1],"word")) word(app,directory);
    else powerpoint(app,directory);
    CALL0(app,"Quit");release(app);CoUninitialize();
    printf("PASS app=%s checks=%u elapsed_ms=%llu\n",argv[1],checks,GetTickCount64()-epoch);
    return 0;
}
