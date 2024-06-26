// This file is part of Golly.
// See docs/License.html for the copyright notice.

/*
    Golly uses an embedded Python interpreter (3.3 or later) to execute scripts.
    Here is the official Python copyright notice:

    Copyright (c) 2001-2020 Python Software Foundation.
    All Rights Reserved.

    Copyright (c) 2000 BeOpen.com.
    All Rights Reserved.

    Copyright (c) 1995-2001 Corporation for National Research Initiatives.
    All Rights Reserved.

    Copyright (c) 1991-1995 Stichting Mathematisch Centrum, Amsterdam.
    All Rights Reserved.
*/

#include "wx/wxprec.h"     // for compilers that support precompilation
#ifndef WX_PRECOMP
    #include "wx/wx.h"     // for all others include the necessary headers
#endif
#include "wx/filename.h"   // for wxFileName
#include "wx/dynlib.h"     // for wxDynamicLibrary

#include <inttypes.h>
#include "bigint.h"
#include "lifealgo.h"
#include "qlifealgo.h"
#include "hlifealgo.h"
#include "readpattern.h"
#include "writepattern.h"

#include "wxgolly.h"       // for wxGetApp, mainptr, viewptr, statusptr
#include "wxmain.h"        // for mainptr->...
#include "wxselect.h"      // for Selection
#include "wxview.h"        // for viewptr->...
#include "wxstatus.h"      // for statusptr->...
#include "wxutils.h"       // for Warning, Note, GetString, etc
#include "wxprefs.h"       // for pythonlib, gollydir, etc
#include "wxhelp.h"        // for ShowHelp
#include "wxundo.h"        // for currlayer->undoredo->...
#include "wxalgos.h"       // for *_ALGO, CreateNewUniverse, etc
#include "wxlayer.h"       // for AddLayer, currlayer, currindex, etc
#include "wxscript.h"      // for inscript, abortmsg, GSF_*, etc
#include "wxpython.h"

// =============================================================================

// We load the Python library at runtime so Golly will start up even if Python
// isn't installed.
// Based on code from Mahogany (mahogany.sourceforge.net) and Vim (www.vim.org).

#if 0 // #ifdef __UNIX__
    // avoid warning on Linux
    #undef _POSIX_C_SOURCE
    #undef _XOPEN_SOURCE
#endif

#ifdef __WXMSW__
    #undef HAVE_SSIZE_T
#endif

// prevent Python.h from adding Python library to link settings
#define USE_DL_EXPORT

// Including the Python header
// Python's stable ABI version >= 3.3 is needed
// in order to support PyArg_ParseTuple
#define Py_LIMITED_API 0x03030000
#define PY_SSIZE_T_CLEAN
// workaround for hard-linking to missing python<ver>_d.lib
// Also needed because Py_LIMITED_API cannot be used with _DEBUG
#ifdef _DEBUG
    #undef _DEBUG
    #include <Python.h>
    #define _DEBUG
#else
    #include <Python.h>
#endif

// declare G_* wrappers for the functions we want to use from Python lib
// The wrappers are function pointers that point to the corresponding
// Python C API functions from the python DLL.
// Some conventions for the names:
// 1. Only declare the functions we need
// 2. Try to use functions from Python3's Stable ABI as much as possible.
// 3. When declaring and using dynamically loaded **FUNCTION POINTERS**,
//    prepend "G_" to the corresponding Python C API function names.
//    Otherwise the functions are masked by <Python.h> and the compiler cannot
//    detect uses-before-declaration.
// 4. N.B. Python **TYPES** and **SINGLETONS** are used without "G_" prefixes.
//    (Because they're unique or static variables inside the header files.)
extern "C"
{
    // startup/shutdown
    void(*G_Py_Initialize)(void) = NULL;
    void(*G_Py_Finalize)(void) = NULL;

    // errors
    PyObject*(*G_PyErr_Occurred)(void) = NULL;
    void(*G_PyErr_SetString)(PyObject*, const char*) = NULL;
    void(*G_PyErr_Print)() = NULL;
    void(*G_PyErr_Fetch)(PyObject**, PyObject**, PyObject**) = NULL;
    void(*G_PyErr_Restore)(PyObject*, PyObject*, PyObject*) = NULL;

    // ints
    long(*G_PyLong_AsLong)(PyObject*) = NULL;
    PyObject*(*G_PyLong_FromLong)(long) = NULL;

    // lists
    PyTypeObject *G_PyList_Type = NULL;  // This should be loaded from the Python DLL directly.
    PyObject*(*G_PyList_New)(Py_ssize_t) = NULL;
    Py_ssize_t(*G_PyList_Size)(PyObject*) = NULL;
    PyObject*(*G_PyList_GetItem)(PyObject*, int) = NULL;
    int(*G_PyList_Append)(PyObject*, PyObject*) = NULL;

    // parsing arguments and building values
    int(*G_PyArg_ParseTuple)(PyObject*, char*, ...) = NULL;
    PyObject*(*G_Py_BuildValue)(const char*, ...) = NULL;

    // general objects
    PyObject*(*G_PyObject_GetAttrString)(PyObject*, const char*) = NULL;

    // Modules
    int(*G_PyImport_AppendInittab)(const char*, PyObject*(*)(void)) = NULL;
    PyObject*(*G_PyImport_ImportModule)(const char*) = NULL;
    PyObject*(*G_PyModule_Create2)(PyModuleDef*, int) = NULL;
    PyObject*(*G_PyModule_GetDict)(PyObject*) = NULL;

    // These functions below are very very probably in the Stable ABI,
    // but I'm not 100% sure.
    // In PEP 384, there was a function called `Py_CompileStringFlags`, [1]
    // but it was renamed to `Py_CompileString` in Python 3.2; [2]
    // [1] https://svn.python.org/projects/python/branches/pep-0384/PC/python3.def
    // [2] https://github.com/python/cpython/commit/0d012f284be829c6217f60523db0e1671b7db9d9
    PyObject*(*G_Py_CompileString)(const char*, const char*, int) = NULL;
    PyObject*(*G_PyEval_EvalCode)(PyObject*, PyObject*, PyObject*) = NULL;
    // `Py_IncRef` and `Py_DecRef` are provided from Python as wrapper functions.
    // Note capitalization. (`Py_(X)INCREF` and `Py_(X)DECREF` are macros.)
    // References:
    // [1] https://docs.python.org/3/c-api/refcounting.html (Read the 2nd to the last paragraph.)
    // [2] https://github.com/python/cpython/blob/2.7/Include/object.h#L864-L865
    // [3] https://github.com/python/cpython/blob/3.8/Include/object.h#L551-L552
    void(*G_Py_IncRef)(PyObject*) = NULL;
    void(*G_Py_DecRef)(PyObject*) = NULL;
}

// These are objects, not functions.
// Thus these probably need refcounting and need to be loaded using the functions above.
// Previous comments say that loading objects directly from DLLs aren't recommended.
extern "C" {
    PyObject *G_PyExc_RuntimeError = NULL;
    PyObject *G_PyExc_KeyboardInterrupt = NULL;
    // The singleton `None` object.
    PyObject *G_Py_None = NULL;
}

// handle for Python lib
static wxDllType pythondll = NULL;

static void FreePythonLib()
{
    if ( pythondll ) {
        wxDynamicLibrary::Unload(pythondll);
        pythondll = NULL;
    }
}

static bool LoadPythonLib()
{
    // load the Python library
    wxDynamicLibrary dynlib;
    
    // if dynlib.Load fails then only see the detailed log error if GollyPrefs sets debug_level=2
    wxLogNull* noLog = new wxLogNull();
    if (debuglevel == 2) delete noLog;

    // the prompt message for loading the Python library
    wxString prompt = _(
        "If Python 3 isn't installed then you'll have to Cancel,\n"
        "otherwise change the version numbers to match the\n"
        "version installed on your system and try again."
    );
#ifdef __WXMSW__
    prompt += _(
        "\n\n"
        "If that fails, search your system for a python3*.dll file\n"
        "and enter the full path to that file."
    );
#endif

    // wxDL_GLOBAL corresponds to RTLD_GLOBAL on macOS/Linux (ignored on Windows) and
    // is needed to avoid an ImportError when importing some modules (eg. time)
    while ( !dynlib.Load(pythonlib, wxDL_NOW | wxDL_VERBATIM | wxDL_GLOBAL) ) {
        // prompt user for a different Python library;
        // on Windows pythonlib should be something like "python39.dll"
        // on Linux it's something like "libpython3.9.so"
        // on Mac OS it's something like "/Library/Frameworks/Python.framework/Versions/3.9/Python"
        Beep();
        wxTextEntryDialog dialog(
            wxGetActiveWindow(),
            prompt,
            _("Could not load the Python library"),
            pythonlib,
            wxOK | wxCANCEL
        );
        int button = dialog.ShowModal();
        if (viewptr) viewptr->ResetMouseDown();
        if (button == wxID_OK) {
            pythonlib = dialog.GetValue();
        } else {
            if (debuglevel != 2) delete noLog;
            return false;
        }
    }
    if (debuglevel != 2) delete noLog;

    // load python function address with symbol "FUNC_NAME"
    // to our function pointer G_FUNC_NAME.
    #define LOAD_PYTHON_SYMBOL(NAME) \
        do { \
            void* ptr = dynlib.GetSymbol(#NAME); \
            if ( !ptr ) { \
                wxString err = \
                    _("The Python library does not have this symbol: ") \
                    + wxString(#NAME); \
                Warning(err); \
                return false; \
            } \
            void** g_ptr_address = (void**)(&G_ ## NAME); \
            *g_ptr_address = ptr; \
        } while(0)

    if ( dynlib.IsLoaded() ) {
        // load all functions used
        LOAD_PYTHON_SYMBOL(Py_Initialize);
        LOAD_PYTHON_SYMBOL(Py_Finalize);
        LOAD_PYTHON_SYMBOL(PyErr_Occurred);
        LOAD_PYTHON_SYMBOL(PyErr_SetString);
        LOAD_PYTHON_SYMBOL(PyErr_Print);
        LOAD_PYTHON_SYMBOL(PyErr_Fetch);
        LOAD_PYTHON_SYMBOL(PyErr_Restore);
        LOAD_PYTHON_SYMBOL(PyLong_AsLong);
        LOAD_PYTHON_SYMBOL(PyLong_FromLong);
        LOAD_PYTHON_SYMBOL(PyList_Type);
        LOAD_PYTHON_SYMBOL(PyList_New);
        LOAD_PYTHON_SYMBOL(PyList_Append);
        LOAD_PYTHON_SYMBOL(PyList_GetItem);
        LOAD_PYTHON_SYMBOL(PyList_Size);
        LOAD_PYTHON_SYMBOL(Py_BuildValue);
        LOAD_PYTHON_SYMBOL(PyArg_ParseTuple);
        LOAD_PYTHON_SYMBOL(PyObject_GetAttrString);
        LOAD_PYTHON_SYMBOL(PyImport_ImportModule);
        LOAD_PYTHON_SYMBOL(PyImport_AppendInittab);
        LOAD_PYTHON_SYMBOL(PyModule_Create2);
        LOAD_PYTHON_SYMBOL(PyModule_GetDict);
        LOAD_PYTHON_SYMBOL(Py_CompileString);
        LOAD_PYTHON_SYMBOL(PyEval_EvalCode);
        LOAD_PYTHON_SYMBOL(Py_IncRef);
        LOAD_PYTHON_SYMBOL(Py_DecRef);
        //
        pythondll = dynlib.Detach();
    }
    
    if ( pythondll == NULL ) {
        // should never happen
        Warning(_("Oh dear, the Python library is not loaded!"));
    }
    
    return pythondll != NULL;
}

static void GetPythonObjects()
{
    // this is needed for escape key to abort scripts via PyExc_KeyboardInterrupt
    // (thanks to https://github.com/vim/vim/blob/master/src/if_python3.c)

    // load the `builtins` module for retrieving builtin objects.
    PyObject* builtins = G_PyImport_ImportModule("builtins");  // New reference.
    // load the objects.
    G_PyExc_RuntimeError = G_PyObject_GetAttrString(builtins, "RuntimeError");
    G_PyExc_KeyboardInterrupt = G_PyObject_GetAttrString(builtins, "KeyboardInterrupt");
    G_Py_None = G_PyObject_GetAttrString(builtins, "None");
    // refcount the objects.
    G_Py_DecRef(G_PyExc_RuntimeError);
    G_Py_DecRef(G_PyExc_KeyboardInterrupt);
    G_Py_DecRef(G_Py_None);
    G_Py_DecRef(builtins);
}

// =============================================================================

// some useful macros

#define G_Py_RETURN_NONE return G_Py_IncRef(G_Py_None), G_Py_None
#define PYTHON_ERROR(msg) { G_PyErr_SetString(G_PyExc_RuntimeError, msg); return NULL; }

#define CheckRGB(r,g,b,cmd)                                            \
    if (r < 0 || r > 255 || g < 0 || g > 255 || g < 0 || g > 255) {    \
        char msg[128];                                                 \
        sprintf(msg, "Bad rgb value in %s: %d,%d,%d", cmd, r, g, b);   \
        PYTHON_ERROR(msg);                                             \
    }

#ifdef __WXMAC__
    // use decomposed UTF8 so fopen will work
    #define FILENAME wxString(filename,wxConvLocal).fn_str()
#else
    #define FILENAME filename
#endif

// use UTF8 for the encoding conversion from Python string to wxString
#define PY_ENC wxConvUTF8

#ifdef AUTORELEASE
    // we need to avoid memory leaks on recent Mac OS versions (probably 10.9+)
    // especially if a script command calls Refresh
    #ifdef __WXMAC__
        #include "wx/osx/private.h" // for wxMacAutoreleasePool
    #endif
    #define AUTORELEASE_POOL wxMacAutoreleasePool pool;
#else
    #define AUTORELEASE_POOL
#endif

// -----------------------------------------------------------------------------

bool PythonScriptAborted()
{
    if (allowcheck) wxGetApp().Poller()->checkevents();
    
    // if user hit escape key then AbortPythonScript has raised an exception
    // and G_PyErr_Occurred will be true; if so, caller must return NULL
    // otherwise Python can abort app with this message:
    // Fatal Python error: unexpected exception during garbage collection
    
    return G_PyErr_Occurred() != NULL;
}

// -----------------------------------------------------------------------------

static void AddTwoInts(PyObject* list, long x, long y)
{
    // append two ints to the given list -- these ints can be:
    // the x,y coords of a live cell in a one-state cell list,
    // or the x,y location of a rect, or the wd,ht of a rect
    PyObject* xo = G_PyLong_FromLong(x);
    PyObject* yo = G_PyLong_FromLong(y);
    G_PyList_Append(list, xo);
    G_PyList_Append(list, yo);
    // must decrement references to avoid Python memory leak
    G_Py_DecRef(xo);
    G_Py_DecRef(yo);
}

// -----------------------------------------------------------------------------

static void AddState(PyObject* list, long s)
{
    // append cell state (possibly dead) to a multi-state cell list
    PyObject* so = G_PyLong_FromLong(s);
    G_PyList_Append(list, so);
    G_Py_DecRef(so);
}

// -----------------------------------------------------------------------------

static void AddPadding(PyObject* list)
{
    // assume list is multi-state and add an extra int if necessary so the list
    // has an odd number of ints (this is how we distinguish multi-state lists
    // from one-state lists -- the latter always have an even number of ints)
    int len = G_PyList_Size(list);
    if (len == 0) return;         // always return [] rather than [0]
    if ((len & 1) == 0) {
        PyObject* padding = G_PyLong_FromLong(0L);
        G_PyList_Append(list, padding);
        G_Py_DecRef(padding);
    }
}

// -----------------------------------------------------------------------------

static void AddCellColor(PyObject* list, long s, long r, long g, long b)
{
    // append state,r,g,b values to given list
    PyObject* so = G_PyLong_FromLong(s);
    PyObject* ro = G_PyLong_FromLong(r);
    PyObject* go = G_PyLong_FromLong(g);
    PyObject* bo = G_PyLong_FromLong(b);
    G_PyList_Append(list, so);
    G_PyList_Append(list, ro);
    G_PyList_Append(list, go);
    G_PyList_Append(list, bo);
    // must decrement references to avoid Python memory leak
    G_Py_DecRef(so);
    G_Py_DecRef(ro);
    G_Py_DecRef(go);
    G_Py_DecRef(bo);
}

// -----------------------------------------------------------------------------

static bool ExtractCellList(PyObject* list, lifealgo* universe, bool shift = false)
{
    // extract cell list from given universe
    if ( !universe->isEmpty() ) {
        bigint top, left, bottom, right;
        universe->findedges(&top, &left, &bottom, &right);
        if ( viewptr->OutsideLimits(top, left, bottom, right) ) {
            G_PyErr_SetString(G_PyExc_RuntimeError, "Universe is too big to extract all cells!");
            return false;
        }
        bool multistate = universe->NumCellStates() > 2;
        int itop = top.toint();
        int ileft = left.toint();
        int ibottom = bottom.toint();
        int iright = right.toint();
        int cx, cy;
        int v = 0;
        int cntr = 0;
        for ( cy=itop; cy<=ibottom; cy++ ) {
            for ( cx=ileft; cx<=iright; cx++ ) {
                int skip = universe->nextcell(cx, cy, v);
                if (skip >= 0) {
                    // found next live cell in this row
                    cx += skip;
                    if (shift) {
                        // shift cells so that top left cell of bounding box is at 0,0
                        AddTwoInts(list, cx - ileft, cy - itop);
                    } else {
                        AddTwoInts(list, cx, cy);
                    }
                    if (multistate) AddState(list, v);
                } else {
                    cx = iright;  // done this row
                }
                cntr++;
                if ((cntr % 4096) == 0 && PythonScriptAborted()) return false;
            }
        }
        if (multistate) AddPadding(list);
    }
    return true;
}

// =============================================================================

// The following py_* routines can be called from Python scripts; some are
// based on code in PLife's lifeint.cc (see http://plife.sourceforge.net/).

static PyObject* py_open(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* filename;
    int remember = 0;
    const char* err;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s|i", &filename, &remember)) return NULL;
    
    err = GSF_open(wxString(filename,PY_ENC), remember);
    if (err) PYTHON_ERROR(err);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_save(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* filename;
    const char* format;
    int remember = 0;
    
    if (!G_PyArg_ParseTuple(args, (char*)"ss|i", &filename, &format, &remember)) return NULL;
    
    const char* err = GSF_save(wxString(filename,PY_ENC), format, remember);
    if (err) PYTHON_ERROR(err);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_opendialog(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* title = "Choose a file";
    const char* filetypes = "All files (*)|*";
    const char* initialdir = "";
    const char* initialfname = "";
    int mustexist = 1;
    
    if (!G_PyArg_ParseTuple(args, (char*)"|ssssi", &title, &filetypes,
                          &initialdir, &initialfname, &mustexist)) return NULL;
    
    wxString wxs_title(title, PY_ENC);
    wxString wxs_filetypes(filetypes, PY_ENC);
    wxString wxs_initialdir(initialdir, PY_ENC);
    wxString wxs_initialfname(initialfname, PY_ENC);
    wxString wxs_result = wxEmptyString;
    
    if (wxs_initialdir.IsEmpty()) wxs_initialdir = wxFileName::GetCwd();
    
    if (wxs_filetypes == wxT("dir")) {
        // let user choose a directory
        wxDirDialog dirdlg(NULL, wxs_title, wxs_initialdir, wxDD_NEW_DIR_BUTTON);
        if (dirdlg.ShowModal() == wxID_OK) {
            wxs_result = dirdlg.GetPath();
            if (wxs_result.Last() != wxFILE_SEP_PATH) wxs_result += wxFILE_SEP_PATH;
        }
    } else {
        // let user choose a file
        wxFileDialog opendlg(NULL, wxs_title, wxs_initialdir, wxs_initialfname, wxs_filetypes,
                             wxFD_OPEN | (mustexist == 0 ? 0 : wxFD_FILE_MUST_EXIST) );
        if (opendlg.ShowModal() == wxID_OK) wxs_result = opendlg.GetPath();
    }
    if (viewptr) viewptr->ResetMouseDown();
    
    return G_Py_BuildValue((char*)"s", (const char*)wxs_result.mb_str(PY_ENC));
}

// -----------------------------------------------------------------------------

static PyObject* py_savedialog(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* title = "Choose a save location and filename";
    const char* filetypes = "All files (*)|*";
    const char* initialdir = "";
    const char* initialfname = "";
    int suppressprompt = 0;
    
    if (!G_PyArg_ParseTuple(args, (char*)"|ssssi", &title, &filetypes,
                          &initialdir, &initialfname, &suppressprompt)) return NULL;
    
    wxString wxs_title(title, PY_ENC);
    wxString wxs_filetypes(filetypes, PY_ENC);
    wxString wxs_initialdir(initialdir, PY_ENC);
    wxString wxs_initialfname(initialfname, PY_ENC);
    
    if (wxs_initialdir.IsEmpty()) wxs_initialdir = wxFileName::GetCwd();
    
    wxFileDialog savedlg( NULL, wxs_title, wxs_initialdir, wxs_initialfname, wxs_filetypes,
                         wxFD_SAVE | (suppressprompt == 0 ? wxFD_OVERWRITE_PROMPT : 0) );
    
    wxString wxs_savefname = wxEmptyString;
    if ( savedlg.ShowModal() == wxID_OK ) wxs_savefname = savedlg.GetPath();
    if (viewptr) viewptr->ResetMouseDown();
    
    return G_Py_BuildValue((char*)"s", (const char*)wxs_savefname.mb_str(PY_ENC));
}

// -----------------------------------------------------------------------------

static PyObject* py_load(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* filename;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &filename)) return NULL;
    
    // create temporary universe of same type as current universe
    lifealgo* tempalgo = CreateNewUniverse(currlayer->algtype, allowcheck);
    // readpattern will call setrule
    
    // read pattern into temporary universe
    const char* err = readpattern(FILENAME, *tempalgo);
    if (err) {
        // try all other algos until readpattern succeeds
        for (int i = 0; i < NumAlgos(); i++) {
            if (i != currlayer->algtype) {
                delete tempalgo;
                tempalgo = CreateNewUniverse(i, allowcheck);
                err = readpattern(FILENAME, *tempalgo);
                if (!err) break;
            }
        }
    }
    
    if (err) {
        delete tempalgo;
        PYTHON_ERROR(err);
    }
    
    // convert pattern into a cell list, shifting cell coords so that the
    // bounding box's top left cell is at 0,0
    PyObject* outlist = G_PyList_New(0);
    bool done = ExtractCellList(outlist, tempalgo, true);
    delete tempalgo;
    if (!done) {
        G_Py_DecRef(outlist);
        return NULL;
    }
    
    return outlist;
}

// -----------------------------------------------------------------------------

static PyObject* py_store(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    PyObject* inlist;
    const char* filename;
    const char* description = NULL; // ignored
    
    if (!G_PyArg_ParseTuple(args, (char*)"O!s|s", G_PyList_Type, &inlist, &filename, &description))
        return NULL;
    
    // create temporary universe of same type as current universe
    lifealgo* tempalgo = CreateNewUniverse(currlayer->algtype, allowcheck);
    const char* err = tempalgo->setrule(currlayer->algo->getrule());
    if (err) tempalgo->setrule(tempalgo->DefaultRule());
    
    if (savexrle) {
        // ensure non-zero generation count will be included in #CXRLE comment
        tempalgo->setGeneration( currlayer->algo->getGeneration() );
    }
    
    // copy cell list into temporary universe
    bool multistate = (G_PyList_Size(inlist) & 1) == 1;
    int ints_per_cell = multistate ? 3 : 2;
    int num_cells = G_PyList_Size(inlist) / ints_per_cell;
    for (int n = 0; n < num_cells; n++) {
        int item = ints_per_cell * n;
        long x = G_PyLong_AsLong( G_PyList_GetItem(inlist, item) );
        long y = G_PyLong_AsLong( G_PyList_GetItem(inlist, item + 1) );
        // check if x,y is outside bounded grid
        const char* err = GSF_checkpos(tempalgo, x, y);
        if (err) { delete tempalgo; PYTHON_ERROR(err); }
        if (multistate) {
            long state = G_PyLong_AsLong( G_PyList_GetItem(inlist, item + 2) );
            if (tempalgo->setcell(x, y, state) < 0) {
                tempalgo->endofpattern();
                delete tempalgo;
                PYTHON_ERROR("store error: state value is out of range.");
            }
        } else {
            tempalgo->setcell(x, y, 1);
        }
        if ((n % 4096) == 0 && PythonScriptAborted()) {
            tempalgo->endofpattern();
            delete tempalgo;
            return NULL;
        }
    }
    tempalgo->endofpattern();
    
    // write pattern to given file in RLE/XRLE format
    bigint top, left, bottom, right;
    tempalgo->findedges(&top, &left, &bottom, &right);
    pattern_format format = savexrle ? XRLE_format : RLE_format;
    // if grid is bounded then force XRLE_format so that position info is recorded
    if (tempalgo->gridwd > 0 || tempalgo->gridht > 0) format = XRLE_format;
    err = writepattern(FILENAME, *tempalgo, format, no_compression,
                       top.toint(), left.toint(), bottom.toint(), right.toint());
    delete tempalgo;
    if (err) PYTHON_ERROR(err);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

// deprecated (use py_getdir)
static PyObject* py_appdir(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"s", (const char*)gollydir.mb_str(PY_ENC));
}

// -----------------------------------------------------------------------------

// deprecated (use py_getdir)
static PyObject* py_datadir(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"s", (const char*)datadir.mb_str(PY_ENC));
}

// -----------------------------------------------------------------------------

static PyObject* py_setdir(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* dirname;
    const char* newdir;
    const char* err;
    
    if (!G_PyArg_ParseTuple(args, (char*)"ss", &dirname, &newdir)) return NULL;
    
    err = GSF_setdir(dirname, wxString(newdir,PY_ENC));
    if (err) PYTHON_ERROR(err);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getdir(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* dirname;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &dirname)) return NULL;
    
    const char* dirstring = GSF_getdir(dirname);
    if (dirstring == NULL) PYTHON_ERROR("getdir error: unknown directory name.");
    
    return G_Py_BuildValue((char*)"s", dirstring);
}

// -----------------------------------------------------------------------------

static PyObject* py_new(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* title;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &title)) return NULL;
    
    mainptr->NewPattern(wxString(title,PY_ENC));
    DoAutoUpdate();
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_cut(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    if (viewptr->SelectionExists()) {
        viewptr->CutSelection();
        DoAutoUpdate();
    } else {
        PYTHON_ERROR("cut error: no selection.");
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_copy(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    if (viewptr->SelectionExists()) {
        viewptr->CopySelection();
        DoAutoUpdate();
    } else {
        PYTHON_ERROR("copy error: no selection.");
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_clear(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int where;
    
    if (!G_PyArg_ParseTuple(args, (char*)"i", &where)) return NULL;
    
    if (viewptr->SelectionExists()) {
        if (where == 0)
            viewptr->ClearSelection();
        else
            viewptr->ClearOutsideSelection();
        DoAutoUpdate();
    } else {
        PYTHON_ERROR("clear error: no selection.");
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_paste(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int x, y;
    const char* mode;
    const char* err;
    
    if (!G_PyArg_ParseTuple(args, (char*)"iis", &x, &y, &mode)) return NULL;
    
    err = GSF_paste(x, y, mode);
    if (err) PYTHON_ERROR(err);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_shrink(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    int remove_if_empty = 0;
    if (!G_PyArg_ParseTuple(args, (char*)"|i", &remove_if_empty)) return NULL;
    
    if (viewptr->SelectionExists()) {
        currlayer->currsel.Shrink(false, remove_if_empty != 0);
                               // false == don't fit in viewport
        DoAutoUpdate();
    } else {
        PYTHON_ERROR("shrink error: no selection.");
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_randfill(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int perc;
    
    if (!G_PyArg_ParseTuple(args, (char*)"i", &perc)) return NULL;
    
    if (perc < 1 || perc > 100) {
        PYTHON_ERROR("randfill error: percentage must be from 1 to 100.");
    }
    
    if (viewptr->SelectionExists()) {
        int oldperc = randomfill;
        randomfill = perc;
        viewptr->RandomFill();
        randomfill = oldperc;
        DoAutoUpdate();
    } else {
        PYTHON_ERROR("randfill error: no selection.");
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_flip(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int direction;
    
    if (!G_PyArg_ParseTuple(args, (char*)"i", &direction)) return NULL;
    
    if (viewptr->SelectionExists()) {
        viewptr->FlipSelection(direction != 0);    // 1 = top-bottom
        DoAutoUpdate();
    } else {
        PYTHON_ERROR("flip error: no selection.");
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_rotate(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int direction;
    
    if (!G_PyArg_ParseTuple(args, (char*)"i", &direction)) return NULL;
    
    if (viewptr->SelectionExists()) {
        viewptr->RotateSelection(direction == 0);    // 0 = clockwise
        DoAutoUpdate();
    } else {
        PYTHON_ERROR("rotate error: no selection.");
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_parse(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* s;
    
    // defaults for optional params
    long x0  = 0;
    long y0  = 0;
    long axx = 1;
    long axy = 0;
    long ayx = 0;
    long ayy = 1;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s|llllll", &s, &x0, &y0, &axx, &axy, &ayx, &ayy))
        return NULL;
    
    PyObject* outlist = G_PyList_New(0);
    
    long x = 0, y = 0;
    
    if (strchr(s, '*')) {
        // parsing 'visual' format
        int c = *s++;
        while (c) {
            switch (c) {
                case '\n': if (x) { x = 0; y++; } break;
                case '.': x++; break;
                case '*':
                    AddTwoInts(outlist, x0 + x * axx + y * axy, y0 + x * ayx + y * ayy);
                    x++;
                    break;
            }
            c = *s++;
        }
    } else {
        // parsing RLE format; first check if multi-state data is present
        bool multistate = false;
        const char* p = s;
        while (*p) {
            char c = *p++;
            if ((c == '.') || ('p' <= c && c <= 'y') || ('A' <= c && c <= 'X')) {
                multistate = true;
                break;
            }
        }
        int prefix = 0;
        bool done = false;
        int c = *s++;
        while (c && !done) {
            if (isdigit(c))
                prefix = 10 * prefix + (c - '0');
            else {
                prefix += (prefix == 0);
                switch (c) {
                    case '!': done = true; break;
                    case '$': x = 0; y += prefix; break;
                    case 'b': x += prefix; break;
                    case '.': x += prefix; break;
                    case 'o':
                        for (int k = 0; k < prefix; k++, x++) {
                            AddTwoInts(outlist, x0 + x * axx + y * axy, y0 + x * ayx + y * ayy);
                            if (multistate) AddState(outlist, 1);
                        }
                        break;
                    default:
                        if (('p' <= c && c <= 'y') || ('A' <= c && c <= 'X')) {
                            // multistate must be true
                            int state;
                            if (c < 'p') {
                                state = c - 'A' + 1;
                            } else {
                                state = 24 * (c - 'p' + 1);
                                c = *s++;
                                if ('A' <= c && c <= 'X') {
                                    state = state + c - 'A' + 1;
                                } else {
                                    // G_Py_DecRef(outlist);
                                    // PYTHON_ERROR("parse error: illegal multi-char state.");
                                    // be more forgiving and treat 'p'..'y' like 'o'
                                    state = 1;
                                    s--;
                                }
                            }
                            for (int k = 0; k < prefix; k++, x++) {
                                AddTwoInts(outlist, x0 + x * axx + y * axy, y0 + x * ayx + y * ayy);
                                AddState(outlist, state);
                            }
                        }
                }
                prefix = 0;
            }
            c = *s++;
        }
        if (multistate) AddPadding(outlist);
    }
    
    return outlist;
}

// -----------------------------------------------------------------------------

static PyObject* py_transform(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    PyObject* inlist;
    long x0, y0;
    
    // defaults for optional params
    long axx = 1;
    long axy = 0;
    long ayx = 0;
    long ayy = 1;
    
    if (!G_PyArg_ParseTuple(args, (char*)"O!ll|llll",
                          G_PyList_Type, &inlist, &x0, &y0, &axx, &axy, &ayx, &ayy))
        return NULL;
    
    PyObject* outlist = G_PyList_New(0);
    
    bool multistate = (G_PyList_Size(inlist) & 1) == 1;
    int ints_per_cell = multistate ? 3 : 2;
    int num_cells = G_PyList_Size(inlist) / ints_per_cell;
    for (int n = 0; n < num_cells; n++) {
        int item = ints_per_cell * n;
        long x = G_PyLong_AsLong( G_PyList_GetItem(inlist, item) );
        long y = G_PyLong_AsLong( G_PyList_GetItem(inlist, item + 1) );
        AddTwoInts(outlist, x0 + x * axx + y * axy,
                   y0 + x * ayx + y * ayy);
        if (multistate) {
            long state = G_PyLong_AsLong( G_PyList_GetItem(inlist, item + 2) );
            AddState(outlist, state);
        }
        if ((n % 4096) == 0 && PythonScriptAborted()) {
            G_Py_DecRef(outlist);
            return NULL;
        }
    }
    if (multistate) AddPadding(outlist);
    
    return outlist;
}

// -----------------------------------------------------------------------------

static PyObject* py_evolve(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int ngens = 0;
    PyObject* inlist;
    
    if (!G_PyArg_ParseTuple(args, (char*)"O!i", G_PyList_Type, &inlist, &ngens)) return NULL;
    
    if (ngens < 0) {
        PYTHON_ERROR("evolve error: number of generations is negative.");
    }
    
    // create a temporary universe of same type as current universe
    lifealgo* tempalgo = CreateNewUniverse(currlayer->algtype, allowcheck);
    const char* err = tempalgo->setrule(currlayer->algo->getrule());
    if (err) tempalgo->setrule(tempalgo->DefaultRule());
    
    // copy cell list into temporary universe
    bool multistate = (G_PyList_Size(inlist) & 1) == 1;
    int ints_per_cell = multistate ? 3 : 2;
    int num_cells = G_PyList_Size(inlist) / ints_per_cell;
    for (int n = 0; n < num_cells; n++) {
        int item = ints_per_cell * n;
        long x = G_PyLong_AsLong( G_PyList_GetItem(inlist, item) );
        long y = G_PyLong_AsLong( G_PyList_GetItem(inlist, item + 1) );
        // check if x,y is outside bounded grid
        const char* err = GSF_checkpos(tempalgo, x, y);
        if (err) { delete tempalgo; PYTHON_ERROR(err); }
        if (multistate) {
            long state = G_PyLong_AsLong( G_PyList_GetItem(inlist, item + 2) );
            if (tempalgo->setcell(x, y, state) < 0) {
                tempalgo->endofpattern();
                delete tempalgo;
                PYTHON_ERROR("evolve error: state value is out of range.");
            }
        } else {
            tempalgo->setcell(x, y, 1);
        }
        if ((n % 4096) == 0 && PythonScriptAborted()) {
            tempalgo->endofpattern();
            delete tempalgo;
            return NULL;
        }
    }
    tempalgo->endofpattern();
    
    // advance pattern by ngens
    mainptr->generating = true;
    if (tempalgo->unbounded && (tempalgo->gridwd > 0 || tempalgo->gridht > 0)) {
        // a bounded grid must use an increment of 1 so we can call
        // CreateBorderCells and DeleteBorderCells around each step()
        tempalgo->setIncrement(1);
        while (ngens > 0) {
            if (PythonScriptAborted()) {
                mainptr->generating = false;
                delete tempalgo;
                return NULL;
            }
            if (!tempalgo->CreateBorderCells()) break;
            tempalgo->step();
            if (!tempalgo->DeleteBorderCells()) break;
            ngens--;
        }
    } else {
        tempalgo->setIncrement(ngens);
        tempalgo->step();
    }
    mainptr->generating = false;
    
    // convert new pattern into a new cell list
    PyObject* outlist = G_PyList_New(0);
    bool done = ExtractCellList(outlist, tempalgo);
    delete tempalgo;
    if (!done) {
        G_Py_DecRef(outlist);
        return NULL;
    }
    
    return outlist;
}

// -----------------------------------------------------------------------------

static const char* BAD_STATE = "putcells error: state value is out of range.";

static PyObject* py_putcells(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    PyObject* list;
    
    // defaults for optional params
    long x0  = 0;
    long y0  = 0;
    long axx = 1;
    long axy = 0;
    long ayx = 0;
    long ayy = 1;
    // default for mode is 'or'; 'xor' mode is also supported;
    // for a one-state list 'copy' mode currently has the same effect as 'or' mode
    // because there is no bounding box to set dead cells, but a multi-state list can
    // have dead cells so in that case 'copy' mode is not the same as 'or' mode
    const char* mode = "or";
    
    if (!G_PyArg_ParseTuple(args, (char*)"O!|lllllls", G_PyList_Type, &list,
                          &x0, &y0, &axx, &axy, &ayx, &ayy, &mode))
        return NULL;
    
    wxString modestr = wxString(mode, PY_ENC);
    if ( !(  modestr.IsSameAs(wxT("or"), false)
           || modestr.IsSameAs(wxT("xor"), false)
           || modestr.IsSameAs(wxT("copy"), false)
           || modestr.IsSameAs(wxT("and"), false)
           || modestr.IsSameAs(wxT("not"), false)) ) {
        PYTHON_ERROR("putcells error: unknown mode.");
    }
    
    // save cell changes if undo/redo is enabled and script isn't constructing a pattern
    bool savecells = allowundo && !currlayer->stayclean;
    // use ChangeCell below and combine all changes due to consecutive setcell/putcells
    // if (savecells) SavePendingChanges();
    
    bool multistate = (G_PyList_Size(list) & 1) == 1;
    int ints_per_cell = multistate ? 3 : 2;
    int num_cells = G_PyList_Size(list) / ints_per_cell;
    bool abort = false;
    const char* err = NULL;
    bool pattchanged = false;
    lifealgo* curralgo = currlayer->algo;
    
    if (modestr.IsSameAs(wxT("copy"), false)) {
        // TODO: find bounds of cell list and call ClearRect here (to be added to wxedit.cpp)
    }
    
    if (modestr.IsSameAs(wxT("and"), false)) {
        if (!curralgo->isEmpty()) {
            int newstate = 1;
            for (int n = 0; n < num_cells; n++) {
                int item = ints_per_cell * n;
                long x = G_PyLong_AsLong( G_PyList_GetItem(list, item) );
                long y = G_PyLong_AsLong( G_PyList_GetItem(list, item + 1) );
                int newx = x0 + x * axx + y * axy;
                int newy = y0 + x * ayx + y * ayy;
                // check if newx,newy is outside bounded grid
                err = GSF_checkpos(curralgo, newx, newy);
                if (err) break;
                int oldstate = curralgo->getcell(newx, newy);
                if (multistate) {
                    // multi-state lists can contain dead cells so newstate might be 0
                    newstate = G_PyLong_AsLong( G_PyList_GetItem(list, item + 2) );
                }
                if (newstate != oldstate && oldstate > 0) {
                    curralgo->setcell(newx, newy, 0);
                    if (savecells) ChangeCell(newx, newy, oldstate, 0);
                    pattchanged = true;
                }
                if ((n % 4096) == 0 && PythonScriptAborted()) {
                    abort = true;
                    break;
                }
            }
        }
    } else if (modestr.IsSameAs(wxT("xor"), false)) {
        // loop code is duplicated here to allow 'or' case to execute faster
        int numstates = curralgo->NumCellStates();
        for (int n = 0; n < num_cells; n++) {
            int item = ints_per_cell * n;
            long x = G_PyLong_AsLong( G_PyList_GetItem(list, item) );
            long y = G_PyLong_AsLong( G_PyList_GetItem(list, item + 1) );
            int newx = x0 + x * axx + y * axy;
            int newy = y0 + x * ayx + y * ayy;
            // check if newx,newy is outside bounded grid
            err = GSF_checkpos(curralgo, newx, newy);
            if (err) break;
            int oldstate = curralgo->getcell(newx, newy);
            int newstate;
            if (multistate) {
                // multi-state lists can contain dead cells so newstate might be 0
                newstate = G_PyLong_AsLong( G_PyList_GetItem(list, item + 2) );
                if (newstate == oldstate) {
                    if (oldstate != 0) newstate = 0;
                } else {
                    newstate = newstate ^ oldstate;
                    // if xor overflows then don't change current state
                    if (newstate >= numstates) newstate = oldstate;
                }
                if (newstate != oldstate) {
                    // paste (possibly transformed) cell into current universe
                    if (curralgo->setcell(newx, newy, newstate) < 0) {
                        G_PyErr_SetString(G_PyExc_RuntimeError, BAD_STATE);
                        abort = true;
                        break;
                    }
                    if (savecells) ChangeCell(newx, newy, oldstate, newstate);
                    pattchanged = true;
                }
            } else {
                // one-state lists only contain live cells
                newstate = 1 - oldstate;
                // paste (possibly transformed) cell into current universe
                if (curralgo->setcell(newx, newy, newstate) < 0) {
                    G_PyErr_SetString(G_PyExc_RuntimeError, BAD_STATE);
                    abort = true;
                    break;
                }
                if (savecells) ChangeCell(newx, newy, oldstate, newstate);
                pattchanged = true;
            }
            if ((n % 4096) == 0 && PythonScriptAborted()) {
                abort = true;
                break;
            }
        }
    } else {
        bool notmode = modestr.IsSameAs(wxT("not"), false);
        bool ormode = modestr.IsSameAs(wxT("or"), false);
        int newstate = notmode ? 0 : 1;
        int maxstate = curralgo->NumCellStates() - 1;
        for (int n = 0; n < num_cells; n++) {
            int item = ints_per_cell * n;
            long x = G_PyLong_AsLong( G_PyList_GetItem(list, item) );
            long y = G_PyLong_AsLong( G_PyList_GetItem(list, item + 1) );
            int newx = x0 + x * axx + y * axy;
            int newy = y0 + x * ayx + y * ayy;
            // check if newx,newy is outside bounded grid
            err = GSF_checkpos(curralgo, newx, newy);
            if (err) break;
            int oldstate = curralgo->getcell(newx, newy);
            if (multistate) {
                // multi-state lists can contain dead cells so newstate might be 0
                newstate = G_PyLong_AsLong( G_PyList_GetItem(list, item + 2) );
                if (notmode) newstate = maxstate - newstate;
                if (ormode && newstate == 0) newstate = oldstate;
            }
            if (newstate != oldstate) {
                // paste (possibly transformed) cell into current universe
                if (curralgo->setcell(newx, newy, newstate) < 0) {
                    G_PyErr_SetString(G_PyExc_RuntimeError, BAD_STATE);
                    abort = true;
                    break;
                }
                if (savecells) ChangeCell(newx, newy, oldstate, newstate);
                pattchanged = true;
            }
            if ((n % 4096) == 0 && PythonScriptAborted()) {
                abort = true;
                break;
            }
        }
    }
    
    if (pattchanged) {
        curralgo->endofpattern();
        MarkLayerDirty();
        DoAutoUpdate();
    }
    
    if (err) PYTHON_ERROR(err);
    if (abort) return NULL;
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getcells(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    PyObject* rect_list;
    
    if (!G_PyArg_ParseTuple(args, (char*)"O!", G_PyList_Type, &rect_list)) return NULL;
    
    // convert pattern in given rect into a cell list
    PyObject* outlist = G_PyList_New(0);
    
    int numitems = G_PyList_Size(rect_list);
    if (numitems == 0) {
        // return empty cell list
    } else if (numitems == 4) {
        int ileft = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 0) );
        int itop = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 1) );
        int wd = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 2) );
        int ht = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 3) );
        const char* err = GSF_checkrect(ileft, itop, wd, ht);
        if (err) {
            G_Py_DecRef(outlist);
            PYTHON_ERROR(err);
        }
        int iright = ileft + wd - 1;
        int ibottom = itop + ht - 1;
        int cx, cy;
        int v = 0;
        int cntr = 0;
        lifealgo* curralgo = currlayer->algo;
        bool multistate = curralgo->NumCellStates() > 2;
        for ( cy=itop; cy<=ibottom; cy++ ) {
            for ( cx=ileft; cx<=iright; cx++ ) {
                int skip = curralgo->nextcell(cx, cy, v);
                if (skip >= 0) {
                    // found next live cell in this row
                    cx += skip;
                    if (cx <= iright) {
                        AddTwoInts(outlist, cx, cy);
                        if (multistate) AddState(outlist, v);
                    }
                } else {
                    cx = iright;  // done this row
                }
                cntr++;
                if ((cntr % 4096) == 0 && PythonScriptAborted()) {
                    G_Py_DecRef(outlist);
                    return NULL;
                }
            }
        }
        if (multistate) AddPadding(outlist);
    } else {
        G_Py_DecRef(outlist);
        PYTHON_ERROR("getcells error: arg must be [] or [x,y,wd,ht].");
    }
    
    return outlist;
}

// -----------------------------------------------------------------------------

static PyObject* py_join(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    PyObject* inlist1;
    PyObject* inlist2;
    
    if (!G_PyArg_ParseTuple(args, (char*)"O!O!", G_PyList_Type, &inlist1, G_PyList_Type, &inlist2))
        return NULL;
    
    bool multi1 = (G_PyList_Size(inlist1) & 1) == 1;
    bool multi2 = (G_PyList_Size(inlist2) & 1) == 1;
    bool multiout = multi1 || multi2;
    int ints_per_cell, num_cells;
    long x, y, state;
    PyObject* outlist = G_PyList_New(0);
    
    // append 1st list
    ints_per_cell = multi1 ? 3 : 2;
    num_cells = G_PyList_Size(inlist1) / ints_per_cell;
    for (int n = 0; n < num_cells; n++) {
        int item = ints_per_cell * n;
        x = G_PyLong_AsLong( G_PyList_GetItem(inlist1, item) );
        y = G_PyLong_AsLong( G_PyList_GetItem(inlist1, item + 1) );
        if (multi1) {
            state = G_PyLong_AsLong( G_PyList_GetItem(inlist1, item + 2) );
        } else {
            state = 1;
        }
        AddTwoInts(outlist, x, y);
        if (multiout) AddState(outlist, state);
        if ((n % 4096) == 0 && PythonScriptAborted()) {
            G_Py_DecRef(outlist);
            return NULL;
        }
    }
    
    // append 2nd list
    ints_per_cell = multi2 ? 3 : 2;
    num_cells = G_PyList_Size(inlist2) / ints_per_cell;
    for (int n = 0; n < num_cells; n++) {
        int item = ints_per_cell * n;
        x = G_PyLong_AsLong( G_PyList_GetItem(inlist2, item) );
        y = G_PyLong_AsLong( G_PyList_GetItem(inlist2, item + 1) );
        if (multi2) {
            state = G_PyLong_AsLong( G_PyList_GetItem(inlist2, item + 2) );
        } else {
            state = 1;
        }
        AddTwoInts(outlist, x, y);
        if (multiout) AddState(outlist, state);
        if ((n % 4096) == 0 && PythonScriptAborted()) {
            G_Py_DecRef(outlist);
            return NULL;
        }
    }
    
    if (multiout) AddPadding(outlist);
    
    return outlist;
}

// -----------------------------------------------------------------------------

static PyObject* py_hash(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    PyObject* rect_list;
    
    if (!G_PyArg_ParseTuple(args, (char*)"O!", G_PyList_Type, &rect_list)) return NULL;
    
    int numitems = G_PyList_Size(rect_list);
    if (numitems != 4) {
        PYTHON_ERROR("hash error: arg must be [x,y,wd,ht].");
    }
    
    int x  = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 0) );
    int y  = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 1) );
    int wd = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 2) );
    int ht = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 3) );
    const char* err = GSF_checkrect(x, y, wd, ht);
    if (err) PYTHON_ERROR(err);
    
    int hash = GSF_hash(x, y, wd, ht);
    
    return G_Py_BuildValue((char*)"i", hash);
}

// -----------------------------------------------------------------------------

static PyObject* py_getclip(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    if (!mainptr->ClipboardHasText()) {
        PYTHON_ERROR("getclip error: no pattern in clipboard.");
    }
    
    // convert pattern in clipboard into a cell list, but where the first 2 items
    // are the pattern's width and height (not necessarily the minimal bounding box
    // because the pattern might have empty borders, or it might even be empty)
    PyObject* outlist = G_PyList_New(0);
    
    // create a temporary layer for storing the clipboard pattern
    Layer* templayer = CreateTemporaryLayer();
    if (!templayer) {
        PYTHON_ERROR("getclip error: failed to create temporary layer.");
    }
    
    // read clipboard pattern into temporary universe and set edges
    // (not a minimal bounding box if pattern is empty or has empty borders)
    bigint top, left, bottom, right;
    if ( viewptr->GetClipboardPattern(templayer, &top, &left, &bottom, &right, false) ) {
        if ( viewptr->OutsideLimits(top, left, bottom, right) ) {
            delete templayer;
            G_Py_DecRef(outlist);
            PYTHON_ERROR("getclip error: pattern is too big.");
        }
        int itop = top.toint();
        int ileft = left.toint();
        int ibottom = bottom.toint();
        int iright = right.toint();
        int wd = iright - ileft + 1;
        int ht = ibottom - itop + 1;
        
        AddTwoInts(outlist, wd, ht);
        
        // extract cells from templayer
        lifealgo* tempalgo = templayer->algo;
        bool multistate = tempalgo->NumCellStates() > 2;
        int cx, cy;
        int cntr = 0;
        int v = 0;
        for ( cy=itop; cy<=ibottom; cy++ ) {
            for ( cx=ileft; cx<=iright; cx++ ) {
                int skip = tempalgo->nextcell(cx, cy, v);
                if (skip >= 0) {
                    // found next live cell in this row
                    cx += skip;
                    // shift cells so that top left cell of bounding box is at 0,0
                    AddTwoInts(outlist, cx - ileft, cy - itop);
                    if (multistate) AddState(outlist, v);
                } else {
                    cx = iright;  // done this row
                }
                cntr++;
                if ((cntr % 4096) == 0 && PythonScriptAborted()) {
                    delete templayer;
                    G_Py_DecRef(outlist);
                    return NULL;
                }
            }
        }
        // if no live cells then return [wd,ht] rather than [wd,ht,0]
        if (multistate && G_PyList_Size(outlist) > 2) {
            AddPadding(outlist);
        }
        
        delete templayer;
    } else {
        // assume error message has been displayed
        delete templayer;
        G_Py_DecRef(outlist);
        return NULL;
    }
    
    return outlist;
}

// -----------------------------------------------------------------------------

static PyObject* py_select(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    PyObject* rect_list;
    
    if (!G_PyArg_ParseTuple(args, (char*)"O!", G_PyList_Type, &rect_list)) return NULL;
    
    int numitems = G_PyList_Size(rect_list);
    if (numitems == 0) {
        // remove any existing selection
        GSF_select(0, 0, 0, 0);
    } else if (numitems == 4) {
        int x  = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 0) );
        int y  = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 1) );
        int wd = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 2) );
        int ht = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 3) );
        const char* err = GSF_checkrect(x, y, wd, ht);
        if (err) PYTHON_ERROR(err);
        // set selection rect
        GSF_select(x, y, wd, ht);
    } else {
        PYTHON_ERROR("select error: arg must be [] or [x,y,wd,ht].");
    }
    
    DoAutoUpdate();
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getrect(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    PyObject* outlist = G_PyList_New(0);
    
    if (!currlayer->algo->isEmpty()) {
        bigint top, left, bottom, right;
        currlayer->algo->findedges(&top, &left, &bottom, &right);
        if ( viewptr->OutsideLimits(top, left, bottom, right) ) {
            G_Py_DecRef(outlist);
            PYTHON_ERROR("getrect error: pattern is too big.");
        }
        long x = left.toint();
        long y = top.toint();
        long wd = right.toint() - x + 1;
        long ht = bottom.toint() - y + 1;
        
        AddTwoInts(outlist, x, y);
        AddTwoInts(outlist, wd, ht);
    }
    
    return outlist;
}

// -----------------------------------------------------------------------------

static PyObject* py_getselrect(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    PyObject* outlist = G_PyList_New(0);
    
    if (viewptr->SelectionExists()) {
        if (currlayer->currsel.TooBig()) {
            G_Py_DecRef(outlist);
            PYTHON_ERROR("getselrect error: selection is too big.");
        }
        int x, y, wd, ht;
        currlayer->currsel.GetRect(&x, &y, &wd, &ht);
        
        AddTwoInts(outlist, x, y);
        AddTwoInts(outlist, wd, ht);
    }
    
    return outlist;
}

// -----------------------------------------------------------------------------

static PyObject* py_setcell(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int x, y, state;
    const char* err;
    
    if (!G_PyArg_ParseTuple(args, (char*)"iii", &x, &y, &state)) return NULL;
    
    err = GSF_setcell(x, y, state);    
    if (err) PYTHON_ERROR(err);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getcell(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int x, y;
    
    if (!G_PyArg_ParseTuple(args, (char*)"ii", &x, &y)) return NULL;
    
    // check if x,y is outside bounded grid
    const char* err = GSF_checkpos(currlayer->algo, x, y);
    if (err) PYTHON_ERROR(err);
    
    return G_Py_BuildValue((char*)"i", currlayer->algo->getcell(x, y));
}

// -----------------------------------------------------------------------------

static PyObject* py_setcursor(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* newcursor;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &newcursor)) return NULL;
    
    const char* oldcursor = CursorToString(currlayer->curs);
    wxCursor* cursptr = StringToCursor(newcursor);
    if (cursptr) {
        viewptr->SetCursorMode(cursptr);
        // see the cursor change, including button in edit bar
        mainptr->UpdateUserInterface();
    } else {
        PYTHON_ERROR("setcursor error: unknown cursor string.");
    }
    
    // return old cursor (simplifies saving and restoring cursor)
    return G_Py_BuildValue((char*)"s", oldcursor);
}

// -----------------------------------------------------------------------------

static PyObject* py_getcursor(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"s", CursorToString(currlayer->curs));
}

// -----------------------------------------------------------------------------

static PyObject* py_empty(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"i", currlayer->algo->isEmpty() ? 1 : 0);
}

// -----------------------------------------------------------------------------

static PyObject* py_run(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int ngens;
    
    if (!G_PyArg_ParseTuple(args, (char*)"i", &ngens)) return NULL;
    
    if (ngens > 0 && !currlayer->algo->isEmpty()) {
        if (ngens > 1) {
            bigint saveinc = currlayer->algo->getIncrement();
            currlayer->algo->setIncrement(ngens);
            mainptr->NextGeneration(true);            // step by ngens
            currlayer->algo->setIncrement(saveinc);
        } else {
            mainptr->NextGeneration(false);           // step 1 gen
        }
        DoAutoUpdate();
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_step(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    if (!currlayer->algo->isEmpty()) {
        mainptr->NextGeneration(true);      // step by current increment
        DoAutoUpdate();
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_setstep(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int exp;
    
    if (!G_PyArg_ParseTuple(args, (char*)"i", &exp)) return NULL;
    
    mainptr->SetStepExponent(exp);
    DoAutoUpdate();
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getstep(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"i", currlayer->currexpo);
}

// -----------------------------------------------------------------------------

static PyObject* py_setbase(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int base;
    
    if (!G_PyArg_ParseTuple(args, (char*)"i", &base)) return NULL;
    
    if (base < 2) base = 2;
    if (base > MAX_BASESTEP) base = MAX_BASESTEP;
    currlayer->currbase = base;
    
    mainptr->SetGenIncrement();
    DoAutoUpdate();
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getbase(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"i", currlayer->currbase);
}

// -----------------------------------------------------------------------------

static PyObject* py_advance(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int where, ngens;
    
    if (!G_PyArg_ParseTuple(args, (char*)"ii", &where, &ngens)) return NULL;
    
    if (ngens > 0) {
        if (viewptr->SelectionExists()) {
            while (ngens > 0) {
                ngens--;
                if (where == 0)
                    currlayer->currsel.Advance();
                else
                    currlayer->currsel.AdvanceOutside();
            }
            DoAutoUpdate();
        } else {
            PYTHON_ERROR("advance error: no selection.");
        }
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_reset(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    if (currlayer->algo->getGeneration() != currlayer->startgen) {
        mainptr->ResetPattern();
        DoAutoUpdate();
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_setgen(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* genstring = NULL;
    const char* err;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &genstring)) return NULL;
    
    err = GSF_setgen(genstring);
    if (err) PYTHON_ERROR(err);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getgen(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    char sepchar = '\0';
    
    if (!G_PyArg_ParseTuple(args, (char*)"|c", &sepchar)) return NULL;
    
    return G_Py_BuildValue((char*)"s", currlayer->algo->getGeneration().tostring(sepchar));
}

// -----------------------------------------------------------------------------

static PyObject* py_getpop(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    char sepchar = '\0';
    
    if (!G_PyArg_ParseTuple(args, (char*)"|c", &sepchar)) return NULL;
    
    return G_Py_BuildValue((char*)"s", currlayer->algo->getPopulation().tostring(sepchar));
}

// -----------------------------------------------------------------------------

static PyObject* py_setalgo(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* algostring = NULL;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &algostring)) return NULL;
    
    const char* err = GSF_setalgo(algostring);
    if (err) PYTHON_ERROR(err);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getalgo(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int index = currlayer->algtype;
    
    if (!G_PyArg_ParseTuple(args, (char*)"|i", &index)) return NULL;
    
    if (index < 0 || index >= NumAlgos()) {
        char msg[64];
        sprintf(msg, "Bad getalgo index: %d", index);
        PYTHON_ERROR(msg);
    }
    
    return G_Py_BuildValue((char*)"s", GetAlgoName(index));
}

// -----------------------------------------------------------------------------

static PyObject* py_setrule(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* rulestring = NULL;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &rulestring)) return NULL;
    
    const char* err = GSF_setrule(rulestring);
    if (err) PYTHON_ERROR(err);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getrule(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"s", currlayer->algo->getrule());
}

// -----------------------------------------------------------------------------

static PyObject* py_getwidth(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"i", currlayer->algo->gridwd);
}

// -----------------------------------------------------------------------------

static PyObject* py_getheight(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"i", currlayer->algo->gridht);
}

// -----------------------------------------------------------------------------

static PyObject* py_numstates(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"i", currlayer->algo->NumCellStates());
}

// -----------------------------------------------------------------------------

static PyObject* py_numalgos(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"i", NumAlgos());
}

// -----------------------------------------------------------------------------

static PyObject* py_setpos(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* x;
    const char* y;
    
    if (!G_PyArg_ParseTuple(args, (char*)"ss", &x, &y)) return NULL;
    
    const char* err = GSF_setpos(x, y);
    if (err) PYTHON_ERROR(err);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getpos(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    char sepchar = '\0';
    
    if (!G_PyArg_ParseTuple(args, (char*)"|c", &sepchar)) return NULL;
    
    bigint bigx, bigy;
    viewptr->GetPos(bigx, bigy);
    
    // return position as x,y tuple
    return G_Py_BuildValue("(s, s)", bigx.tostring(sepchar), bigy.tostring(sepchar));
}

// -----------------------------------------------------------------------------

static PyObject* py_setmag(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int mag;
    
    if (!G_PyArg_ParseTuple(args, (char*)"i", &mag)) return NULL;
    
    viewptr->SetMag(mag);
    DoAutoUpdate();
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getmag(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"i", viewptr->GetMag());
}

// -----------------------------------------------------------------------------

static PyObject* py_fit(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    viewptr->FitPattern();
    DoAutoUpdate();
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_fitsel(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    if (viewptr->SelectionExists()) {
        viewptr->FitSelection();
        DoAutoUpdate();
    } else {
        PYTHON_ERROR("fitsel error: no selection.");
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_visrect(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    PyObject* rect_list;
    
    if (!G_PyArg_ParseTuple(args, (char*)"O!", G_PyList_Type, &rect_list)) return NULL;
    
    int numitems = G_PyList_Size(rect_list);
    if (numitems != 4) {
        PYTHON_ERROR("visrect error: arg must be [x,y,wd,ht].");
    }
    
    int x = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 0) );
    int y = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 1) );
    int wd = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 2) );
    int ht = G_PyLong_AsLong( G_PyList_GetItem(rect_list, 3) );
    const char* err = GSF_checkrect(x, y, wd, ht);
    if (err) PYTHON_ERROR(err);
    
    bigint left = x;
    bigint top = y;
    bigint right = x + wd - 1;
    bigint bottom = y + ht - 1;
    int visible = viewptr->CellVisible(left, top) &&
                  viewptr->CellVisible(right, bottom);
    
    return G_Py_BuildValue((char*)"i", visible);
}

// -----------------------------------------------------------------------------

static PyObject* py_setview(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int wd, ht;
    
    if (!G_PyArg_ParseTuple(args, (char*)"ii", &wd, &ht)) return NULL;
    if (wd < 0) wd = 0;
    if (ht < 0) ht = 0;
    
    int currwd, currht;
    bigview->GetClientSize(&currwd, &currht);
    if (currwd < 0) currwd = 0;
    if (currht < 0) currht = 0;
    
    int mainwd, mainht;
    mainptr->GetSize(&mainwd, &mainht);
    mainptr->SetSize(mainwd + (wd - currwd), mainht + (ht - currht));
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getview(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;

    int currwd, currht;
    bigview->GetClientSize(&currwd, &currht);
    if (currwd < 0) currwd = 0;
    if (currht < 0) currht = 0;
    
    // return viewport size as wd,ht tuple
    return G_Py_BuildValue("(i, i)", currwd, currht);
}

// -----------------------------------------------------------------------------

static PyObject* py_update(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    GSF_update();
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_autoupdate(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int flag;
    
    if (!G_PyArg_ParseTuple(args, (char*)"i", &flag)) return NULL;
    
    autoupdate = (flag != 0);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_addlayer(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    if (numlayers >= MAX_LAYERS) {
        PYTHON_ERROR("addlayer error: no more layers can be added.");
    } else {
        AddLayer();
        DoAutoUpdate();
    }
    
    // return index of new layer
    return G_Py_BuildValue((char*)"i", currindex);
}

// -----------------------------------------------------------------------------

static PyObject* py_clone(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    if (numlayers >= MAX_LAYERS) {
        PYTHON_ERROR("clone error: no more layers can be added.");
    } else {
        CloneLayer();
        DoAutoUpdate();
    }
    
    // return index of new layer
    return G_Py_BuildValue((char*)"i", currindex);
}

// -----------------------------------------------------------------------------

static PyObject* py_duplicate(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    if (numlayers >= MAX_LAYERS) {
        PYTHON_ERROR("duplicate error: no more layers can be added.");
    } else {
        DuplicateLayer();
        DoAutoUpdate();
    }
    
    // return index of new layer
    return G_Py_BuildValue((char*)"i", currindex);
}

// -----------------------------------------------------------------------------

static PyObject* py_dellayer(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    if (numlayers <= 1) {
        PYTHON_ERROR("dellayer error: there is only one layer.");
    } else {
        DeleteLayer();
        DoAutoUpdate();
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_movelayer(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int fromindex, toindex;
    
    if (!G_PyArg_ParseTuple(args, (char*)"ii", &fromindex, &toindex)) return NULL;
    
    if (fromindex < 0 || fromindex >= numlayers) {
        char msg[64];
        sprintf(msg, "Bad movelayer fromindex: %d", fromindex);
        PYTHON_ERROR(msg);
    }
    if (toindex < 0 || toindex >= numlayers) {
        char msg[64];
        sprintf(msg, "Bad movelayer toindex: %d", toindex);
        PYTHON_ERROR(msg);
    }
    
    MoveLayer(fromindex, toindex);
    DoAutoUpdate();
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_setlayer(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int index;
    
    if (!G_PyArg_ParseTuple(args, (char*)"i", &index)) return NULL;
    
    if (index < 0 || index >= numlayers) {
        char msg[64];
        sprintf(msg, "Bad setlayer index: %d", index);
        PYTHON_ERROR(msg);
    }
    
    SetLayer(index);
    DoAutoUpdate();
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getlayer(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"i", currindex);
}

// -----------------------------------------------------------------------------

static PyObject* py_numlayers(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"i", numlayers);
}

// -----------------------------------------------------------------------------

static PyObject* py_maxlayers(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    return G_Py_BuildValue((char*)"i", MAX_LAYERS);
}

// -----------------------------------------------------------------------------

static PyObject* py_setname(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* name;
    int index = currindex;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s|i", &name, &index)) return NULL;
    
    if (index < 0 || index >= numlayers) {
        char msg[64];
        sprintf(msg, "Bad setname index: %d", index);
        PYTHON_ERROR(msg);
    }
    
    GSF_setname(wxString(name,PY_ENC), index);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getname(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int index = currindex;
    
    if (!G_PyArg_ParseTuple(args, (char*)"|i", &index)) return NULL;
    
    if (index < 0 || index >= numlayers) {
        char msg[64];
        sprintf(msg, "Bad getname index: %d", index);
        PYTHON_ERROR(msg);
    }
    
    return G_Py_BuildValue((char*)"s", (const char*)GetLayer(index)->currname.mb_str(PY_ENC));
}

// -----------------------------------------------------------------------------

static PyObject* py_query(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);

    const char* qstr;
    const char* mstr;
    const char* ly = "Yes";
    const char* ln = "No";
    const char* lc = "Cancel";
    
    if (!G_PyArg_ParseTuple(args, (char*)"ss|sss", &qstr, &mstr, &ly, &ln, &lc)) return NULL;
    
    wxString query(qstr, PY_ENC);
    wxString msg(mstr, PY_ENC);
    wxString labelYes(ly, PY_ENC);
    wxString labelNo(ln, PY_ENC);
    wxString labelCancel(lc, PY_ENC);
    
    long style = wxICON_QUESTION | wxCENTER | wxYES_NO;
    if (labelCancel.length() > 0) style = style | wxCANCEL; // add Cancel button
    
    wxMessageDialog dialog(wxGetActiveWindow(), msg, query, style);
    
    if (labelCancel.length() > 0) {
        dialog.SetYesNoCancelLabels(labelYes, labelNo, labelCancel);
    } else {
        dialog.SetYesNoLabels(labelYes, labelNo);
    }
    
    wxString label;
    bool finished = false;
    while (!finished) {
        finished = true;
        int button = dialog.ShowModal();
        if (viewptr) viewptr->ResetMouseDown();
        switch (button) {
            case wxID_YES: {
                label = dialog.GetYesLabel();
                break;
            }
            case wxID_NO: {
                label = dialog.GetNoLabel();
                break;
            }
            case wxID_CANCEL: {
                label = dialog.GetCancelLabel();
                if (labelCancel.length() == 0 && label == wxString("gtk-cancel")) {
                    // this happens on Linux if the dialog has no Cancel button
                    // but the user closed it by hitting escape, so we show the
                    // dialog again to force them to select one of the 2 buttons
                    finished = false;
                }
                break;
            }
            default: {
                // should never happen
                PYTHON_ERROR("query bug: unexpected button.");
            }
        }
    }

    return G_Py_BuildValue((char*)"s", (const char*)label.mb_str(PY_ENC));
}

// -----------------------------------------------------------------------------

static PyObject* py_setcolors(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    PyObject* color_list;
    
    if (!G_PyArg_ParseTuple(args, (char*)"O!", G_PyList_Type, &color_list)) return NULL;
    
    int len = G_PyList_Size(color_list);
    if (len == 0) {
        // restore default colors in current layer and its clones
        UpdateLayerColors();
    } else if (len == 6) {
        // create gradient from r1,g1,b1 to r2,g2,b2
        int r1 = G_PyLong_AsLong( G_PyList_GetItem(color_list, 0) );
        int g1 = G_PyLong_AsLong( G_PyList_GetItem(color_list, 1) );
        int b1 = G_PyLong_AsLong( G_PyList_GetItem(color_list, 2) );
        int r2 = G_PyLong_AsLong( G_PyList_GetItem(color_list, 3) );
        int g2 = G_PyLong_AsLong( G_PyList_GetItem(color_list, 4) );
        int b2 = G_PyLong_AsLong( G_PyList_GetItem(color_list, 5) );
        CheckRGB(r1, g1, b1, "setcolors");
        CheckRGB(r2, g2, b2, "setcolors");
        currlayer->fromrgb.Set(r1, g1, b1);
        currlayer->torgb.Set(r2, g2, b2);
        CreateColorGradient();
        UpdateIconColors();
        UpdateCloneColors();
    } else if (len % 4 == 0) {
        int i = 0;
        while (i < len) {
            int s = G_PyLong_AsLong( G_PyList_GetItem(color_list, i) ); i++;
            int r = G_PyLong_AsLong( G_PyList_GetItem(color_list, i) ); i++;
            int g = G_PyLong_AsLong( G_PyList_GetItem(color_list, i) ); i++;
            int b = G_PyLong_AsLong( G_PyList_GetItem(color_list, i) ); i++;
            CheckRGB(r, g, b, "setcolors");
            if (s == -1) {
                // set all LIVE states to r,g,b (best not to alter state 0)
                for (s = 1; s < currlayer->algo->NumCellStates(); s++) {
                    currlayer->cellr[s] = r;
                    currlayer->cellg[s] = g;
                    currlayer->cellb[s] = b;
                }
            } else {
                if (s < 0 || s >= currlayer->algo->NumCellStates()) {
                    char msg[64];
                    sprintf(msg, "Bad state in setcolors: %d", s);
                    PYTHON_ERROR(msg);
                } else {
                    currlayer->cellr[s] = r;
                    currlayer->cellg[s] = g;
                    currlayer->cellb[s] = b;
                }
            }
        }
        UpdateIconColors();
        UpdateCloneColors();
    } else {
        PYTHON_ERROR("setcolors error: list length is not a multiple of 4.");
    }
    
    DoAutoUpdate();
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getcolors(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int state = -1;
    
    if (!G_PyArg_ParseTuple(args, (char*)"|i", &state)) return NULL;
    
    PyObject* outlist = G_PyList_New(0);
    
    if (state == -1) {
        // return colors for ALL states, including state 0
        for (state = 0; state < currlayer->algo->NumCellStates(); state++) {
            AddCellColor(outlist, state, currlayer->cellr[state],
                         currlayer->cellg[state],
                         currlayer->cellb[state]);
        }
    } else if (state >= 0 && state < currlayer->algo->NumCellStates()) {
        AddCellColor(outlist, state, currlayer->cellr[state],
                     currlayer->cellg[state],
                     currlayer->cellb[state]);
    } else {
        char msg[64];
        sprintf(msg, "Bad getcolors state: %d", state);
        PYTHON_ERROR(msg);
    }
    
    return outlist;
}

// -----------------------------------------------------------------------------

static PyObject* py_os(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;

    return G_Py_BuildValue((char*)"s", GSF_os());
}

// -----------------------------------------------------------------------------

static PyObject* py_sound(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    const char* cmd = "";
    const char* soundfile = "";
    float volume = 1.0;
    if (!G_PyArg_ParseTuple(args, (char*)"|ssf", &cmd, &soundfile, &volume)) return NULL;

    #ifdef ENABLE_SOUND
        // check for sound
        if (cmd[0] == 0) {
            if (GSF_SoundEnabled()) {
                // sound is enabled and initialized
                return G_Py_BuildValue((char*)"i", 2);
            } else {
                // failed to initialize sound
                return G_Py_BuildValue((char*)"i", 1);
            }
        }
        
        // check which sound command is specified
        const char* result = NULL;
        if        (strcmp(cmd, "play") == 0) {      result = GSF_SoundPlay(soundfile, volume, false);
        } else if (strcmp(cmd, "loop") == 0) {      result = GSF_SoundPlay(soundfile, volume, true);
        } else if (strcmp(cmd, "stop") == 0) {      result = GSF_SoundStop(soundfile);
        } else if (strcmp(cmd, "state") == 0) {     result = GSF_SoundState(soundfile);
        } else if (strcmp(cmd, "volume") == 0) {    result = GSF_SoundVolume(soundfile, volume);
        } else if (strcmp(cmd, "pause") == 0) {     result = GSF_SoundPause(soundfile);
        } else if (strcmp(cmd, "resume") == 0) {    result = GSF_SoundResume(soundfile);
        } else {
            PYTHON_ERROR("unknown sound command");
        }
        
        if (result == NULL) {
            G_Py_RETURN_NONE;
        } else {
            // check if GSF_SoundPlay etc returned an error msg
            if (result[0] == 'E' && result[1] == 'R' && result[2] == 'R' ) {
                std::string msg = result + 4; // skip past "ERR:"
                PYTHON_ERROR(msg.c_str());
            }
            return G_Py_BuildValue((char*)"s", result); // should be from GSF_SoundState
        }
    #else
        // sound support is not enabled
        return G_Py_BuildValue((char*)"i", 0);
    #endif
}

// -----------------------------------------------------------------------------

static PyObject* py_setoption(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* optname;
    int oldval, newval;
    
    if (!G_PyArg_ParseTuple(args, (char*)"si", &optname, &newval)) return NULL;
    
    if (!GSF_setoption(optname, newval, &oldval)) {
        PYTHON_ERROR("setoption error: unknown option.");
    }
    
    // return old value (simplifies saving and restoring settings)
    return G_Py_BuildValue((char*)"i", oldval);
}

// -----------------------------------------------------------------------------

static PyObject* py_getoption(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* optname;
    int optval;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &optname)) return NULL;
    
    if (!GSF_getoption(optname, &optval)) {
        PYTHON_ERROR("getoption error: unknown option.");
    }
    
    return G_Py_BuildValue((char*)"i", optval);
}

// -----------------------------------------------------------------------------

static PyObject* py_setcolor(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* colname;
    int r, g, b;
    
    if (!G_PyArg_ParseTuple(args, (char*)"siii", &colname, &r, &g, &b)) return NULL;
    
    wxColor newcol(r, g, b);
    wxColor oldcol;
    
    if (!GSF_setcolor(colname, newcol, oldcol)) {
        PYTHON_ERROR("setcolor error: unknown color.");
    }
    
    // return old r,g,b values (simplifies saving and restoring colors)
    return G_Py_BuildValue("(i, i, i)", oldcol.Red(), oldcol.Green(), oldcol.Blue());
}

// -----------------------------------------------------------------------------

static PyObject* py_getcolor(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* colname;
    wxColor color;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &colname)) return NULL;
    
    if (!GSF_getcolor(colname, color)) {
        PYTHON_ERROR("getcolor error: unknown color.");
    }
    
    // return r,g,b tuple
    return G_Py_BuildValue("(i, i, i)", color.Red(), color.Green(), color.Blue());
}

// -----------------------------------------------------------------------------

static PyObject* py_setclipstr(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* clipstr;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &clipstr)) return NULL;
    
    wxString wxs_clip(clipstr, PY_ENC);
    mainptr->CopyTextToClipboard(wxs_clip);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getclipstr(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    wxTextDataObject data;
    if (!mainptr->GetTextFromClipboard(&data)) {
        data.SetText(wxEmptyString);
    }
    
    wxString wxs_clipstr = data.GetText();
    return G_Py_BuildValue((char*)"s", (const char*)wxs_clipstr.mb_str(PY_ENC));
}

// -----------------------------------------------------------------------------

static PyObject* py_getstring(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* prompt;
    const char* initial = "";
    const char* title = "";
    
    if (!G_PyArg_ParseTuple(args, (char*)"s|ss", &prompt, &initial, &title))
        return NULL;
    
    wxString result;
    if ( !GetString(wxString(title,PY_ENC), wxString(prompt,PY_ENC),
                    wxString(initial,PY_ENC), result) ) {
        // user hit Cancel button
        AbortPythonScript();
        return NULL;
    }
    
    return G_Py_BuildValue((char*)"s", (const char*)result.mb_str(PY_ENC));
}

// -----------------------------------------------------------------------------

static PyObject* py_getxy(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    statusptr->CheckMouseLocation(mainptr->infront);   // sets mousepos
    
    if (viewptr->showcontrols) mousepos = wxEmptyString;
    
    return G_Py_BuildValue((char*)"s", (const char*)mousepos.mb_str(PY_ENC));
}

// -----------------------------------------------------------------------------

static PyObject* py_getevent(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    int get = 1;
    
    if (!G_PyArg_ParseTuple(args, (char*)"|i", &get)) return NULL;
    
    wxString event;
    GSF_getevent(event, get);
    
    return G_Py_BuildValue((char*)"s", (const char*)event.mb_str(PY_ENC));
}

// -----------------------------------------------------------------------------

static PyObject* py_doevent(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* event;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &event)) return NULL;
    
    if (event[0]) {
        const char* err = GSF_doevent(wxString(event,PY_ENC));
        if (err) PYTHON_ERROR(err);
    }
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getkey(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    
    if (!G_PyArg_ParseTuple(args, (char*)"")) return NULL;
    
    char s[2];        // room for char + NULL
    s[0] = GSF_getkey();
    s[1] = '\0';
    
    return G_Py_BuildValue((char*)"s", s);
}

// -----------------------------------------------------------------------------

static PyObject* py_dokey(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* ascii;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &ascii)) return NULL;
    
    GSF_dokey(ascii);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_show(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* s = NULL;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &s)) return NULL;
    
    inscript = false;
    statusptr->DisplayMessage(wxString(s,PY_ENC));
    inscript = true;
    // make sure status bar is visible
    if (!showstatus) mainptr->ToggleStatusBar();
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_error(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* s = NULL;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &s)) return NULL;
    
    inscript = false;
    statusptr->ErrorMessage(wxString(s,PY_ENC));
    inscript = true;
    // make sure status bar is visible
    if (!showstatus) mainptr->ToggleStatusBar();
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_warn(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* s = NULL;
    int showCancel = 1;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s|i", &s, &showCancel)) return NULL;
    
    Warning(wxString(s,PY_ENC), showCancel != 0);

    // if user hit Cancel then best to abort *now*
    if (showCancel != 0 && PythonScriptAborted()) return NULL;
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_note(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* s = NULL;
    int showCancel = 1;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s|i", &s, &showCancel)) return NULL;
    
    Note(wxString(s,PY_ENC), showCancel != 0);

    // if user hit Cancel then best to abort *now*
    if (showCancel != 0 && PythonScriptAborted()) return NULL;
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_help(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* htmlfile = NULL;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &htmlfile)) return NULL;
    
    ShowHelp(wxString(htmlfile,PY_ENC));
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_check(PyObject* self, PyObject* args)
{
    // no need for AUTORELEASE_POOL here
    // if (PythonScriptAborted()) return NULL;
    // don't call checkevents() here otherwise we can't safely write code like
    //    if g.getlayer() == target:
    //       g.check(0)
    //       ... do stuff to target layer ...
    //       g.check(1)
    wxUnusedVar(self);
    int flag;
    
    if (!G_PyArg_ParseTuple(args, (char*)"i", &flag)) return NULL;
    
    allowcheck = (flag != 0);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_exit(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* err = NULL;
    
    if (!G_PyArg_ParseTuple(args, (char*)"|s", &err)) return NULL;
    
    GSF_exit(wxString(err, PY_ENC));
    AbortPythonScript();
    
    // exception raised so must return NULL
    return NULL;
}

// -----------------------------------------------------------------------------

static PyObject* py_stderr(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    // probably safer not to call checkevents here
    // if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);
    const char* s = NULL;
    
    if (!G_PyArg_ParseTuple(args, (char*)"s", &s)) return NULL;
    
    // accumulate stderr messages in global string (shown after script finishes)
    scripterr = wxString(s, PY_ENC);
    
    G_Py_RETURN_NONE;
}

// -----------------------------------------------------------------------------

static PyObject* py_getinfo(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);

    const char* comments = GSF_getinfo();
    
    return G_Py_BuildValue((char*)"s", comments);
}

// -----------------------------------------------------------------------------

static PyObject* py_getpath(PyObject* self, PyObject* args)
{
    AUTORELEASE_POOL
    if (PythonScriptAborted()) return NULL;
    wxUnusedVar(self);

    const char* path = GSF_getpath();
    
    return G_Py_BuildValue((char*)"s", path);
}

// -----------------------------------------------------------------------------

static PyMethodDef py_methods[] = {
    // filing
    { "open",         py_open,       METH_VARARGS, "open given pattern file" },
    { "save",         py_save,       METH_VARARGS, "save pattern in given file using given format" },
    { "opendialog",   py_opendialog, METH_VARARGS, "return input path and filename chosen by user" },
    { "savedialog",   py_savedialog, METH_VARARGS, "return output path and filename chosen by user" },
    { "load",         py_load,       METH_VARARGS, "read pattern file and return cell list" },
    { "store",        py_store,      METH_VARARGS, "write cell list to a file (in RLE format)" },
    { "setdir",       py_setdir,     METH_VARARGS, "set location of specified directory" },
    { "getdir",       py_getdir,     METH_VARARGS, "return location of specified directory" },
    { "getpath",      py_getpath,    METH_VARARGS, "return the path to the current opened pattern" },
    { "getinfo",      py_getinfo,    METH_VARARGS, "return comments from pattern file" },
    // next two are deprecated (use getdir)
    { "appdir",       py_appdir,     METH_VARARGS, "return location of Golly app" },
    { "datadir",      py_datadir,    METH_VARARGS, "return location of user-specific data" },
    // editing
    { "new",          py_new,        METH_VARARGS, "create new universe and set window title" },
    { "cut",          py_cut,        METH_VARARGS, "cut selection to clipboard" },
    { "copy",         py_copy,       METH_VARARGS, "copy selection to clipboard" },
    { "clear",        py_clear,      METH_VARARGS, "clear inside/outside selection" },
    { "paste",        py_paste,      METH_VARARGS, "paste clipboard pattern at x,y using given mode" },
    { "shrink",       py_shrink,     METH_VARARGS, "shrink selection" },
    { "randfill",     py_randfill,   METH_VARARGS, "randomly fill selection to given percentage" },
    { "flip",         py_flip,       METH_VARARGS, "flip selection top-bottom or left-right" },
    { "rotate",       py_rotate,     METH_VARARGS, "rotate selection 90 deg clockwise or anticlockwise" },
    { "parse",        py_parse,      METH_VARARGS, "parse RLE or Life 1.05 string and return cell list" },
    { "transform",    py_transform,  METH_VARARGS, "apply an affine transformation to cell list" },
    { "evolve",       py_evolve,     METH_VARARGS, "generate pattern contained in given cell list" },
    { "putcells",     py_putcells,   METH_VARARGS, "paste given cell list into current universe" },
    { "getcells",     py_getcells,   METH_VARARGS, "return cell list in given rectangle" },
    { "join",         py_join,       METH_VARARGS, "return concatenation of given cell lists" },
    { "hash",         py_hash,       METH_VARARGS, "return hash value for pattern in given rectangle" },
    { "getclip",      py_getclip,    METH_VARARGS, "return pattern in clipboard (as cell list)" },
    { "select",       py_select,     METH_VARARGS, "select [x, y, wd, ht] rectangle or remove if []" },
    { "getrect",      py_getrect,    METH_VARARGS, "return pattern rectangle as [] or [x, y, wd, ht]" },
    { "getselrect",   py_getselrect, METH_VARARGS, "return selection rectangle as [] or [x, y, wd, ht]" },
    { "setcell",      py_setcell,    METH_VARARGS, "set given cell to given state" },
    { "getcell",      py_getcell,    METH_VARARGS, "get state of given cell" },
    { "setcursor",    py_setcursor,  METH_VARARGS, "set cursor (returns old cursor)" },
    { "getcursor",    py_getcursor,  METH_VARARGS, "return current cursor" },
    // control
    { "empty",        py_empty,      METH_VARARGS, "return true if universe is empty" },
    { "run",          py_run,        METH_VARARGS, "run current pattern for given number of gens" },
    { "step",         py_step,       METH_VARARGS, "run current pattern for current step" },
    { "setstep",      py_setstep,    METH_VARARGS, "set step exponent" },
    { "getstep",      py_getstep,    METH_VARARGS, "return current step exponent" },
    { "setbase",      py_setbase,    METH_VARARGS, "set base step" },
    { "getbase",      py_getbase,    METH_VARARGS, "return current base step" },
    { "advance",      py_advance,    METH_VARARGS, "advance inside/outside selection by given gens" },
    { "reset",        py_reset,      METH_VARARGS, "restore starting pattern" },
    { "setgen",       py_setgen,     METH_VARARGS, "set current generation to given string" },
    { "getgen",       py_getgen,     METH_VARARGS, "return current generation as string" },
    { "getpop",       py_getpop,     METH_VARARGS, "return current population as string" },
    { "numstates",    py_numstates,  METH_VARARGS, "return number of cell states in current universe" },
    { "numalgos",     py_numalgos,   METH_VARARGS, "return number of algorithms" },
    { "setalgo",      py_setalgo,    METH_VARARGS, "set current algorithm using given string" },
    { "getalgo",      py_getalgo,    METH_VARARGS, "return name of given or current algorithm" },
    { "setrule",      py_setrule,    METH_VARARGS, "set current rule using given string" },
    { "getrule",      py_getrule,    METH_VARARGS, "return current rule" },
    { "getwidth",     py_getwidth,   METH_VARARGS, "return width of universe (0 if unbounded)" },
    { "getheight",    py_getheight,  METH_VARARGS, "return height of universe (0 if unbounded)" },
    // viewing
    { "setpos",       py_setpos,     METH_VARARGS, "move given cell to middle of viewport" },
    { "getpos",       py_getpos,     METH_VARARGS, "return x,y position of cell in middle of viewport" },
    { "setmag",       py_setmag,     METH_VARARGS, "set magnification (0=1:1, 1=1:2, -1=2:1, etc)" },
    { "getmag",       py_getmag,     METH_VARARGS, "return current magnification" },
    { "fit",          py_fit,        METH_VARARGS, "fit entire pattern in viewport" },
    { "fitsel",       py_fitsel,     METH_VARARGS, "fit selection in viewport" },
    { "visrect",      py_visrect,    METH_VARARGS, "return true if given rect is completely visible" },
    { "setview",      py_setview,    METH_VARARGS, "set pixel dimensions of viewport" },
    { "getview",      py_getview,    METH_VARARGS, "get pixel dimensions of viewport" },
    { "update",       py_update,     METH_VARARGS, "update display (viewport and status bar)" },
    { "autoupdate",   py_autoupdate, METH_VARARGS, "update display after each change to universe?" },
    // layers
    { "addlayer",     py_addlayer,   METH_VARARGS, "add a new layer" },
    { "clone",        py_clone,      METH_VARARGS, "add a cloned layer (shares universe)" },
    { "duplicate",    py_duplicate,  METH_VARARGS, "add a duplicate layer (copies universe)" },
    { "dellayer",     py_dellayer,   METH_VARARGS, "delete current layer" },
    { "movelayer",    py_movelayer,  METH_VARARGS, "move given layer to new index" },
    { "setlayer",     py_setlayer,   METH_VARARGS, "switch to given layer" },
    { "getlayer",     py_getlayer,   METH_VARARGS, "return index of current layer" },
    { "numlayers",    py_numlayers,  METH_VARARGS, "return current number of layers" },
    { "maxlayers",    py_maxlayers,  METH_VARARGS, "return maximum number of layers" },
    { "setname",      py_setname,    METH_VARARGS, "set name of given layer" },
    { "getname",      py_getname,    METH_VARARGS, "get name of given layer" },
    { "setcolors",    py_setcolors,  METH_VARARGS, "set color(s) used in current layer" },
    { "getcolors",    py_getcolors,  METH_VARARGS, "get color(s) used in current layer" },
    // miscellaneous
    { "os",           py_os,         METH_VARARGS, "get the current OS (Windows/Mac/Linux)" },
    { "sound",        py_sound,      METH_VARARGS, "control playing of audio" },
    { "query",        py_query,      METH_VARARGS, "show a query dialog and return answer" },
    { "setoption",    py_setoption,  METH_VARARGS, "set given option to new value (returns old value)" },
    { "getoption",    py_getoption,  METH_VARARGS, "return current value of given option" },
    { "setcolor",     py_setcolor,   METH_VARARGS, "set given color to new r,g,b (returns old r,g,b)" },
    { "getcolor",     py_getcolor,   METH_VARARGS, "return r,g,b values of given color" },
    { "setclipstr",   py_setclipstr, METH_VARARGS, "set the clipboard contents to a given string value" },
    { "getclipstr",   py_getclipstr, METH_VARARGS, "retrieve the contents of the clipboard as a string" },
    { "getstring",    py_getstring,  METH_VARARGS, "display dialog box to get string from user" },
    { "getxy",        py_getxy,      METH_VARARGS, "return current grid location of mouse" },
    { "getevent",     py_getevent,   METH_VARARGS, "return keyboard/mouse event or empty string if none" },
    { "doevent",      py_doevent,    METH_VARARGS, "pass given keyboard/mouse event to Golly to handle" },
    // next two are deprecated (use getevent and doevent)
    { "getkey",       py_getkey,     METH_VARARGS, "return key hit by user or empty string if none" },
    { "dokey",        py_dokey,      METH_VARARGS, "pass given key to Golly's standard key handler" },
    { "show",         py_show,       METH_VARARGS, "show given string in status bar" },
    { "error",        py_error,      METH_VARARGS, "beep and show given string in status bar" },
    { "warn",         py_warn,       METH_VARARGS, "show given string in warning dialog" },
    { "note",         py_note,       METH_VARARGS, "show given string in note dialog" },
    { "help",         py_help,       METH_VARARGS, "show given HTML file in help window" },
    { "check",        py_check,      METH_VARARGS, "allow event checking?" },
    { "exit",         py_exit,       METH_VARARGS, "exit script with optional error message" },
    // for internal use (don't document)
    { "stderr",       py_stderr,     METH_VARARGS, "save Python error message" },
    { NULL, NULL, 0, NULL }
};

// Python 3 port. References:
// [1] https://docs.python.org/3/howto/cporting.html
// [2] https://python3porting.com/cextensions.html#module-initialization
// [3] https://docs.python.org/3/extending/embedding.html
static PyModuleDef py_module = {
    PyModuleDef_HEAD_INIT,
    "golly",
    NULL,
    -1,
    py_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

static PyObject* PyInit_golly(void) {
    // Args: PyModule_Create2(moduledef, module_api_version)
    // PYTHON_API_VERSION has been 1013 since 2.5,
    // so this should be compatible with all versions of Python 3.
    return G_PyModule_Create2(&py_module, 1013);
}

// =============================================================================

static bool pyinited = false;     // InitPython has been successfully called?

int G_PyRun_SimpleString(const char* command) {
    // IMHO **not** short-circuiting is cleaner when you build
    // multiple dependent `PyObject*`s that you have to clean up.
    PyObject* main = NULL;
    PyObject* main_dict = NULL;
    PyObject* code = NULL;
    PyObject* result = NULL;
    // Docs for `Py_CompileString` and `Py_file_input`:
    // https://docs.python.org/3/c-api/veryhigh.html
    code = G_Py_CompileString(command, "wxpython.cpp", Py_file_input);  // New ref.
    // Get the main namespace as there might not be an existing frame.
    if (code != NULL) { main = G_PyImport_ImportModule("__main__"); }  // New ref.
    if (main != NULL) { main_dict = G_PyModule_GetDict(main); } // Borrowed ref.
    if (main_dict != NULL) { result = G_PyEval_EvalCode(code, main_dict, main_dict); } // New ref.
    // Take care of any exceptions
    // PyObject* err_type;
    // PyObject* err_value;
    // PyObject* err_traceback;
    // G_PyErr_Fetch(&err_type, &err_value, &err_traceback);
    // ... some golly-specific error logging...
    // G_PyErr_Restore(err_type, err_value, err_traceback);
    if (G_PyErr_Occurred() != NULL) { G_PyErr_Print(); }
    // Cleanup and return status.
    int status = (result != NULL) ? 0 : -1;
    G_Py_DecRef(result);
    G_Py_DecRef(main);
    G_Py_DecRef(code);
    return status;
}

bool InitPython()
{
    if (!pyinited) {
        // try to load Python library
        if (!LoadPythonLib()) return false;
        
        // Python 3 requires this to be before G_Py_Initialize
        G_PyImport_AppendInittab("golly", PyInit_golly);
        
        // only initialize the Python interpreter once, mainly because multiple
        // G_Py_Initialize/G_Py_Finalize calls cause leaks of about 12K each time!
        G_Py_Initialize();
        
        GetPythonObjects();
        
        // catch Python messages sent to stderr and pass them to py_stderr
        if (G_PyRun_SimpleString(
                "import golly\n"
                "import sys\n"
                "class StderrCatcherForGolly:\n"
                "    def __init__(self):\n"
                "        self.data = ''\n"
                "    def write(self, stuff):\n"
                "        self.data += stuff\n"
                "        golly.stderr(self.data)\n"
                "sys.stderr = StderrCatcherForGolly()\n"
                
                // also create dummy sys.argv so scripts can import Tkinter
                "sys.argv = ['golly-app']\n"
                // works, but Golly's menus get permanently changed on Mac
                ) < 0
            ) Warning(_("StderrCatcherForGolly code failed!"));
        
        // build absolute path to Scripts/Python folder and add to Python's
        // import search list so scripts can import glife from anywhere
        wxString scriptsdir = gollydir + _("Scripts");
        #ifdef __WXMAC__
            // use decomposed UTF8 so interpreter can find path with non-ASCII chars
            scriptsdir = wxString(scriptsdir.fn_str(),wxConvLocal);
        #endif
        scriptsdir += wxFILE_SEP_PATH;
        scriptsdir += _("Python");
        // convert any \ to \\ and then convert any ' to \'
        scriptsdir.Replace(wxT("\\"), wxT("\\\\"));
        scriptsdir.Replace(wxT("'"), wxT("\\'"));
        wxString command = wxT("import sys ; sys.path.append('") + scriptsdir + wxT("')");
        
        // also insert script's current directory at start of sys.path
        // since that's what most Python interpreters do (thanks to Joel Snyder)
        command += wxT(" ; sys.path.insert(0,'')");
        if (G_PyRun_SimpleString(command.mb_str(wxConvLocal)) < 0)
            Warning(_("Failed to append Scripts path!"));
        
        pyinited = true;
    } else {
        // G_Py_Initialize has already been successfully called;
        // G_Py_Finalize is not used to close stderr so reset it here
        if (G_PyRun_SimpleString("import sys ; sys.stderr = StderrCatcherForGolly()\n") < 0)
            Warning(_("G_PyRun_SimpleString failed!"));
    }
    
    return true;
}

// -----------------------------------------------------------------------------

void RunPythonScript(const wxString& filepath)
{
    if (!InitPython()) return;
    
    // we must convert any backslashes to "\\" to avoid "\a" being treated as
    // escape char, then we must escape any apostrophes
    wxString fpath = filepath;
    fpath.Replace(wxT("\\"), wxT("\\\\"));
    fpath.Replace(wxT("'"), wxT("\\'"));
    
    // execute the given script; note that we pass an empty dictionary
    // for the global namespace so that this script cannot change the
    // globals of a caller script (which is possible now that RunScript
    // is re-entrant)
    wxString command = wxT("with open('") + fpath +
        wxT("') as f:\n   code = compile(f.read(), '") + fpath +
        wxT("', 'exec')\n   exec(code, {})");

    G_PyRun_SimpleString(command.mb_str(wxConvLocal));
    // don't use wxConvUTF8 in above line because caller has already converted
    // filepath to decomposed UTF8 if on a Mac
    
    // note that G_PyRun_SimpleString returns -1 if an exception occurred;
    // the error message (in scripterr) is checked at the end of RunScript
}

// -----------------------------------------------------------------------------

void AbortPythonScript()
{
    // raise an exception with a special message
    G_PyErr_SetString(G_PyExc_KeyboardInterrupt, abortmsg);
}

// -----------------------------------------------------------------------------

void FinishPythonScripting()
{
    // G_Py_Finalize can cause an obvious delay, so best not to call it
    // if (pyinited) G_Py_Finalize();
    
    // probably don't really need this either
    FreePythonLib();
}
