/* -*- Mode: C; ; c-file-style: "k&r"; c-basic-offset: 4 -*- */
/* gst-python
 * Copyright (C) 2002 David I. Lehn
 * Copyright (C) 2012 Thibault Saunier <thibault.saunier@collabora.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David I. Lehn <dlehn@users.sourceforge.net>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* include this first, before NO_IMPORT_PYGOBJECT is defined */
#include <Python.h>
#include <pygobject.h>
#include <gst/gst.h>

#include <locale.h>

#define PYGLIB_MODULE_START(symbol, modname)            \
DL_EXPORT(void) init##symbol(void)          \
{                                                       \
    PyObject *module;                                   \
    module = Py_InitModule(modname, symbol##_functions);
#define PYGLIB_MODULE_END }

GST_DEBUG_CATEGORY_STATIC (python_debug);
GST_DEBUG_CATEGORY_STATIC (pygst_debug);
#define GST_CAT_DEFAULT pygst_debug

static PyObject *
gi_gst_fraction_from_value (const GValue * value)
{
  PyObject *module, *dict, *fraction_type, *args, *fraction;
  gint numerator, denominator;

  numerator = gst_value_get_fraction_numerator (value);
  denominator = gst_value_get_fraction_denominator (value);

  module = PyImport_ImportModule ("gi.repository.Gst");
  dict = PyModule_GetDict (module);
  /* For some reson we need this intermediary step */
  module = PyMapping_GetItemString (dict, "_overrides_module");
  dict = PyModule_GetDict (module);
  fraction_type = PyMapping_GetItemString (dict, "Fraction");

  args = Py_BuildValue ("(ii)", numerator, denominator);
  fraction = PyObject_Call (fraction_type, args, NULL);
  Py_DECREF (args);
  Py_DECREF (fraction_type);
  Py_DECREF (module);

  return fraction;
}

static int
gi_gst_fraction_to_value (GValue * value, PyObject * object)
{
  PyObject *numerator, *denominator;

  numerator = PyObject_GetAttrString (object, "num");
  if (numerator == NULL)
    goto fail;

  denominator = PyObject_GetAttrString (object, "denom");
  if (denominator == NULL)
    goto fail;

  gst_value_set_fraction (value,
      PyLong_AsLong (numerator), PyLong_AsLong (denominator));

  return 0;

fail:
  return -1;
}

void
gi_gst_register_types (PyObject * d)
{
  pyg_register_gtype_custom (GST_TYPE_FRACTION,
      gi_gst_fraction_from_value, gi_gst_fraction_to_value);
}

static int
add_templates (gpointer gclass, PyObject * templates)
{
  if (PyTuple_Check (templates)) {
    gint i, len;
    PyGObject *templ;

    len = PyTuple_Size (templates);
    if (len == 0)
      return 0;

    for (i = 0; i < len; i++) {
      templ = (PyGObject *) PyTuple_GetItem (templates, i);
      if (GST_IS_PAD_TEMPLATE (pygobject_get (templ)) == FALSE) {
        PyErr_SetString (PyExc_TypeError,
            "entries for __gsttemplates__ must be of type GstPadTemplate");
        return -1;
      }
    }

    for (i = 0; i < len; i++) {
      templ = (PyGObject *) PyTuple_GetItem (templates, i);
      gst_element_class_add_pad_template (gclass,
          GST_PAD_TEMPLATE (templ->obj));
    }
    return 0;

  }

  if (GST_IS_PAD_TEMPLATE (pygobject_get (templates)) == FALSE) {
    PyErr_SetString (PyExc_TypeError,
        "entry for __gsttemplates__ must be of type GstPadTemplate");
    return -1;
  }

  gst_element_class_add_pad_template (gclass,
      GST_PAD_TEMPLATE (pygobject_get (templates)));

  return 0;
}

static int
_pygst_element_set_metadata (gpointer gclass, PyObject * metadata)
{

  const gchar *longname, *classification, *description, *author;

  if (!PyTuple_Check (metadata)) {
    PyErr_SetString (PyExc_TypeError, "__gstmetadata__ must be a tuple");
    return -1;
  }
  if (PyTuple_Size (metadata) != 4) {
    PyErr_SetString (PyExc_TypeError,
        "__gstmetadata__ must contain 4 elements");
    return -1;
  }
  if (!PyArg_ParseTuple (metadata, "ssss", &longname, &classification,
          &description, &author)) {
    PyErr_SetString (PyExc_TypeError, "__gstmetadata__ must contain 4 strings");
    return -1;
  }
  GST_DEBUG
      ("setting metadata on gclass %p from __gstmetadata__, longname %s",
      gclass, longname);

  gst_element_class_set_metadata (gclass, longname, classification,
      description, author);
  return 0;
}

static int
_pygst_element_init (gpointer gclass, PyTypeObject * pyclass)
{
  PyObject *templates, *metadata;

  GST_DEBUG ("_pygst_element_init for gclass %p", gclass);
  templates = PyDict_GetItemString (pyclass->tp_dict, "__gsttemplates__");
  if (templates) {
    if (add_templates (gclass, templates) != 0)
      return -1;
  } else {
    PyErr_Clear ();
  }
  metadata = PyDict_GetItemString (pyclass->tp_dict, "__gstmetadata__");
  if (metadata) {
    if (_pygst_element_set_metadata (gclass, metadata) != 0)
      return -1;
    PyDict_DelItemString (pyclass->tp_dict, "__gstmetadata__");
  } else {
    PyErr_Clear ();
  }

  return 0;
}

#include <frameobject.h>

static PyObject *
pygst_debug_log (PyObject * pyobject, PyObject * string, GstDebugLevel level,
    gboolean isgstobject)
{
#ifndef GST_DISABLE_GST_DEBUG
  gchar *str;
  gchar *function;
  gchar *filename;
  int lineno;
  PyFrameObject *frame;
  GObject *object = NULL;

  if (!PyArg_ParseTuple (string, "s:gst.debug_log", &str)) {
    PyErr_SetString (PyExc_TypeError, "Need a string!");
    return NULL;
  }

  frame = PyEval_GetFrame ();
  function = PyString_AsString (frame->f_code->co_name);
  filename =
      g_path_get_basename (PyString_AsString (frame->f_code->co_filename));
  lineno = PyCode_Addr2Line (frame->f_code, frame->f_lasti);
  /* gst_debug_log : category, level, file, function, line, object, format, va_list */
  if (isgstobject)
    object = G_OBJECT (pygobject_get (pyobject));
  gst_debug_log (python_debug, level, filename, function, lineno, object,
      "%s", str);
  if (filename)
    g_free (filename);
#endif
  Py_INCREF (Py_None);
  return Py_None;
}

static PyObject *
_wrap_gst_log (PyObject * whatever, PyObject * string)
{
  return pygst_debug_log (whatever, string, GST_LEVEL_LOG, FALSE);
}

static PyObject *
_wrap_gst_debug (PyObject * whatever, PyObject * string)
{
  return pygst_debug_log (whatever, string, GST_LEVEL_DEBUG, FALSE);
}

static PyObject *
_wrap_gst_info (PyObject * whatever, PyObject * string)
{
  return pygst_debug_log (whatever, string, GST_LEVEL_INFO, FALSE);
}

static PyObject *
_wrap_gst_warning (PyObject * whatever, PyObject * string)
{
  return pygst_debug_log (whatever, string, GST_LEVEL_WARNING, FALSE);
}

static PyObject *
_wrap_gst_error (PyObject * whatever, PyObject * string)
{
  return pygst_debug_log (whatever, string, GST_LEVEL_ERROR, FALSE);
}

static PyObject *
_wrap_gst_fixme (PyObject * whatever, PyObject * string)
{
  return pygst_debug_log (whatever, string, GST_LEVEL_FIXME, FALSE);
}

static PyObject *
_wrap_gst_memdump (PyObject * whatever, PyObject * string)
{
  return pygst_debug_log (whatever, string, GST_LEVEL_MEMDUMP, FALSE);
}

static PyMethodDef _gi_gst_functions[] = {
  {"log", (PyCFunction) _wrap_gst_log, METH_VARARGS,
      NULL},
  {"debug", (PyCFunction) _wrap_gst_debug, METH_VARARGS,
      NULL},
  {"info", (PyCFunction) _wrap_gst_info, METH_VARARGS,
      NULL},
  {"warning", (PyCFunction) _wrap_gst_warning, METH_VARARGS,
      NULL},
  {"error", (PyCFunction) _wrap_gst_error, METH_VARARGS,
      NULL},
  {"fixme", (PyCFunction) _wrap_gst_fixme, METH_VARARGS,
      NULL},
  {"memdump", (PyCFunction) _wrap_gst_memdump, METH_VARARGS,
      NULL}
};

PYGLIB_MODULE_START (_gi_gst, "_gi_gst")
{
  PyObject *d;

  /* gst should have been initialized already */

  /* Initialize debugging category */
  GST_DEBUG_CATEGORY_INIT (pygst_debug, "pygst", 0,
      "GStreamer python bindings");
  GST_DEBUG_CATEGORY_INIT (python_debug, "python", GST_DEBUG_FG_GREEN,
      "python code using gst-python");

  pygobject_init (3, 0, 0);

  d = PyModule_GetDict (module);
  gi_gst_register_types (d);
  pyg_register_class_init (GST_TYPE_ELEMENT, _pygst_element_init);
}

PYGLIB_MODULE_END;
