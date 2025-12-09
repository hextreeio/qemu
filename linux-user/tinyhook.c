/*
 * TinyHook - Python-based syscall hooking for QEMU linux-user
 *
 * Copyright (c) 2024
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "tinyhook.h"
#include "qemu.h"

#ifdef CONFIG_TINYHOOK
#define PY_SSIZE_T_CLEAN
#include <Python.h>

static bool g_tinyhook_enabled = false;
static PyObject *g_module = NULL;
static PyObject *g_pre_syscall_hooks = NULL;   /* dict: syscall_num -> callable */
static PyObject *g_post_syscall_hooks = NULL;  /* dict: syscall_num -> callable */

/* Constants exposed to Python */
#define TINYHOOK_ACTION_CONTINUE 0
#define TINYHOOK_ACTION_SKIP 1

/*
 * Python API: tinyhook.register_pre_hook(syscall_num, callback)
 *
 * Register a pre-syscall hook. The callback receives:
 *   callback(syscall_num, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8)
 *
 * And should return a dict with optional keys:
 *   - "action": CONTINUE (0) or SKIP (1)
 *   - "args": tuple of 8 arguments (for modified args)
 *   - "ret": return value (only used if action == SKIP)
 */
static PyObject *py_register_pre_hook(PyObject *self, PyObject *args)
{
    int syscall_num;
    PyObject *callback;

    if (!PyArg_ParseTuple(args, "iO", &syscall_num, &callback)) {
        return NULL;
    }

    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "callback must be callable");
        return NULL;
    }

    PyObject *key = PyLong_FromLong(syscall_num);
    if (!key) {
        return NULL;
    }

    Py_INCREF(callback);
    if (PyDict_SetItem(g_pre_syscall_hooks, key, callback) < 0) {
        Py_DECREF(key);
        Py_DECREF(callback);
        return NULL;
    }
    Py_DECREF(key);

    Py_RETURN_NONE;
}

/*
 * Python API: tinyhook.register_post_hook(syscall_num, callback)
 *
 * Register a post-syscall hook. The callback receives:
 *   callback(syscall_num, ret, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8)
 *
 * And should return the (possibly modified) return value.
 */
static PyObject *py_register_post_hook(PyObject *self, PyObject *args)
{
    int syscall_num;
    PyObject *callback;

    if (!PyArg_ParseTuple(args, "iO", &syscall_num, &callback)) {
        return NULL;
    }

    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "callback must be callable");
        return NULL;
    }

    PyObject *key = PyLong_FromLong(syscall_num);
    if (!key) {
        return NULL;
    }

    Py_INCREF(callback);
    if (PyDict_SetItem(g_post_syscall_hooks, key, callback) < 0) {
        Py_DECREF(key);
        Py_DECREF(callback);
        return NULL;
    }
    Py_DECREF(key);

    Py_RETURN_NONE;
}

/*
 * Python API: tinyhook.unregister_pre_hook(syscall_num)
 */
static PyObject *py_unregister_pre_hook(PyObject *self, PyObject *args)
{
    int syscall_num;

    if (!PyArg_ParseTuple(args, "i", &syscall_num)) {
        return NULL;
    }

    PyObject *key = PyLong_FromLong(syscall_num);
    if (!key) {
        return NULL;
    }

    PyDict_DelItem(g_pre_syscall_hooks, key);
    PyErr_Clear(); /* Ignore KeyError if not found */
    Py_DECREF(key);

    Py_RETURN_NONE;
}

/*
 * Python API: tinyhook.unregister_post_hook(syscall_num)
 */
static PyObject *py_unregister_post_hook(PyObject *self, PyObject *args)
{
    int syscall_num;

    if (!PyArg_ParseTuple(args, "i", &syscall_num)) {
        return NULL;
    }

    PyObject *key = PyLong_FromLong(syscall_num);
    if (!key) {
        return NULL;
    }

    PyDict_DelItem(g_post_syscall_hooks, key);
    PyErr_Clear(); /* Ignore KeyError if not found */
    Py_DECREF(key);

    Py_RETURN_NONE;
}

/*
 * Python API: tinyhook.read_memory(addr, size) -> bytes
 *
 * Read guest memory at the given address.
 */
static PyObject *py_read_memory(PyObject *self, PyObject *args)
{
    unsigned long long addr;
    Py_ssize_t size;

    if (!PyArg_ParseTuple(args, "Kn", &addr, &size)) {
        return NULL;
    }

    if (size <= 0) {
        PyErr_SetString(PyExc_ValueError, "size must be positive");
        return NULL;
    }

    void *host_ptr = g2h_untagged(addr);
    if (!host_ptr) {
        PyErr_SetString(PyExc_MemoryError, "invalid guest address");
        return NULL;
    }

    return PyBytes_FromStringAndSize(host_ptr, size);
}

/*
 * Python API: tinyhook.write_memory(addr, data)
 *
 * Write data to guest memory at the given address.
 */
static PyObject *py_write_memory(PyObject *self, PyObject *args)
{
    unsigned long long addr;
    Py_buffer buffer;

    if (!PyArg_ParseTuple(args, "Ky*", &addr, &buffer)) {
        return NULL;
    }

    void *host_ptr = g2h_untagged(addr);
    if (!host_ptr) {
        PyBuffer_Release(&buffer);
        PyErr_SetString(PyExc_MemoryError, "invalid guest address");
        return NULL;
    }

    memcpy(host_ptr, buffer.buf, buffer.len);
    PyBuffer_Release(&buffer);

    Py_RETURN_NONE;
}

/*
 * Python API: tinyhook.read_string(addr) -> str
 *
 * Read a null-terminated string from guest memory.
 */
static PyObject *py_read_string(PyObject *self, PyObject *args)
{
    unsigned long long addr;

    if (!PyArg_ParseTuple(args, "K", &addr)) {
        return NULL;
    }

    char *host_ptr = g2h_untagged(addr);
    if (!host_ptr) {
        PyErr_SetString(PyExc_MemoryError, "invalid guest address");
        return NULL;
    }

    /* Safely get the string length */
    ssize_t len = target_strlen(addr);
    if (len < 0) {
        PyErr_SetString(PyExc_MemoryError, "invalid string address");
        return NULL;
    }

    return PyUnicode_FromStringAndSize(host_ptr, len);
}

static PyMethodDef tinyhook_methods[] = {
    {"register_pre_hook", py_register_pre_hook, METH_VARARGS,
     "Register a pre-syscall hook: register_pre_hook(syscall_num, callback)"},
    {"register_post_hook", py_register_post_hook, METH_VARARGS,
     "Register a post-syscall hook: register_post_hook(syscall_num, callback)"},
    {"unregister_pre_hook", py_unregister_pre_hook, METH_VARARGS,
     "Unregister a pre-syscall hook: unregister_pre_hook(syscall_num)"},
    {"unregister_post_hook", py_unregister_post_hook, METH_VARARGS,
     "Unregister a post-syscall hook: unregister_post_hook(syscall_num)"},
    {"read_memory", py_read_memory, METH_VARARGS,
     "Read guest memory: read_memory(addr, size) -> bytes"},
    {"write_memory", py_write_memory, METH_VARARGS,
     "Write guest memory: write_memory(addr, data)"},
    {"read_string", py_read_string, METH_VARARGS,
     "Read null-terminated string from guest memory: read_string(addr) -> str"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef tinyhook_module = {
    PyModuleDef_HEAD_INIT,
    "tinyhook",
    "QEMU TinyHook syscall hooking module",
    -1,
    tinyhook_methods
};

static PyObject *PyInit_tinyhook(void)
{
    PyObject *m = PyModule_Create(&tinyhook_module);
    if (!m) {
        return NULL;
    }

    /* Add constants */
    PyModule_AddIntConstant(m, "CONTINUE", TINYHOOK_ACTION_CONTINUE);
    PyModule_AddIntConstant(m, "SKIP", TINYHOOK_ACTION_SKIP);

    return m;
}

int tinyhook_init(const char *script_path)
{
    FILE *fp;

    /* Register the tinyhook module before Py_Initialize */
    if (PyImport_AppendInittab("tinyhook", PyInit_tinyhook) == -1) {
        fprintf(stderr, "tinyhook: failed to register module\n");
        return -1;
    }

    Py_Initialize();

    if (!Py_IsInitialized()) {
        fprintf(stderr, "tinyhook: failed to initialize Python\n");
        return -1;
    }

    /* Create the hook dictionaries */
    g_pre_syscall_hooks = PyDict_New();
    g_post_syscall_hooks = PyDict_New();
    if (!g_pre_syscall_hooks || !g_post_syscall_hooks) {
        fprintf(stderr, "tinyhook: failed to create hook dictionaries\n");
        tinyhook_shutdown();
        return -1;
    }

    /* Import the tinyhook module so it's available */
    g_module = PyImport_ImportModule("tinyhook");
    if (!g_module) {
        fprintf(stderr, "tinyhook: failed to import tinyhook module\n");
        PyErr_Print();
        tinyhook_shutdown();
        return -1;
    }

    /* Add the script's directory to sys.path */
    char *script_dir = g_path_get_dirname(script_path);
    PyObject *sys_path = PySys_GetObject("path");
    if (sys_path && script_dir) {
        PyObject *dir_obj = PyUnicode_FromString(script_dir);
        if (dir_obj) {
            PyList_Insert(sys_path, 0, dir_obj);
            Py_DECREF(dir_obj);
        }
    }
    g_free(script_dir);

    /* Execute the user's script */
    fp = fopen(script_path, "r");
    if (!fp) {
        fprintf(stderr, "tinyhook: failed to open script '%s': %s\n",
                script_path, strerror(errno));
        tinyhook_shutdown();
        return -1;
    }

    PyObject *main_module = PyImport_AddModule("__main__");
    PyObject *main_dict = PyModule_GetDict(main_module);

    /* Make tinyhook module available in __main__ */
    PyDict_SetItemString(main_dict, "tinyhook", g_module);

    PyObject *result = PyRun_FileEx(fp, script_path, Py_file_input,
                                    main_dict, main_dict, 1);
    if (!result) {
        fprintf(stderr, "tinyhook: error executing script '%s':\n", script_path);
        PyErr_Print();
        tinyhook_shutdown();
        return -1;
    }
    Py_DECREF(result);

    g_tinyhook_enabled = true;
    fprintf(stderr, "tinyhook: loaded script '%s'\n", script_path);
    return 0;
}

void tinyhook_shutdown(void)
{
    if (g_tinyhook_enabled) {
        Py_XDECREF(g_pre_syscall_hooks);
        Py_XDECREF(g_post_syscall_hooks);
        Py_XDECREF(g_module);
        g_pre_syscall_hooks = NULL;
        g_post_syscall_hooks = NULL;
        g_module = NULL;
        g_tinyhook_enabled = false;

        if (Py_IsInitialized()) {
            Py_Finalize();
        }
    }
}

bool tinyhook_enabled(void)
{
    return g_tinyhook_enabled;
}

bool tinyhook_pre_syscall(CPUArchState *cpu_env, int num,
                          abi_long arg1, abi_long arg2, abi_long arg3,
                          abi_long arg4, abi_long arg5, abi_long arg6,
                          abi_long arg7, abi_long arg8,
                          TinyHookResult *result)
{
    if (!g_tinyhook_enabled || !g_pre_syscall_hooks) {
        return false;
    }

    /* Initialize result with defaults */
    result->action = TINYHOOK_CONTINUE;
    result->args[0] = arg1;
    result->args[1] = arg2;
    result->args[2] = arg3;
    result->args[3] = arg4;
    result->args[4] = arg5;
    result->args[5] = arg6;
    result->args[6] = arg7;
    result->args[7] = arg8;
    result->ret = 0;

    PyObject *key = PyLong_FromLong(num);
    if (!key) {
        PyErr_Clear();
        return false;
    }

    PyObject *callback = PyDict_GetItem(g_pre_syscall_hooks, key);
    Py_DECREF(key);

    if (!callback) {
        return false;
    }

    /* Call the Python callback */
    PyObject *py_result = PyObject_CallFunction(callback, "iLLLLLLLL",
                                                num,
                                                (long long)arg1,
                                                (long long)arg2,
                                                (long long)arg3,
                                                (long long)arg4,
                                                (long long)arg5,
                                                (long long)arg6,
                                                (long long)arg7,
                                                (long long)arg8);
    if (!py_result) {
        fprintf(stderr, "tinyhook: error in pre-syscall hook for syscall %d:\n", num);
        PyErr_Print();
        return false;
    }

    /* Parse the result */
    if (PyDict_Check(py_result)) {
        /* Check for "action" key */
        PyObject *action = PyDict_GetItemString(py_result, "action");
        if (action && PyLong_Check(action)) {
            long action_val = PyLong_AsLong(action);
            if (action_val == TINYHOOK_ACTION_SKIP) {
                result->action = TINYHOOK_SKIP;
            }
        }

        /* Check for "args" key */
        PyObject *args = PyDict_GetItemString(py_result, "args");
        if (args && PyTuple_Check(args) && PyTuple_Size(args) == 8) {
            for (int i = 0; i < 8; i++) {
                PyObject *item = PyTuple_GetItem(args, i);
                if (item && PyLong_Check(item)) {
                    result->args[i] = (abi_long)PyLong_AsLongLong(item);
                }
            }
        }

        /* Check for "ret" key */
        PyObject *ret = PyDict_GetItemString(py_result, "ret");
        if (ret && PyLong_Check(ret)) {
            result->ret = (abi_long)PyLong_AsLongLong(ret);
        }
    }

    Py_DECREF(py_result);
    return true;
}

abi_long tinyhook_post_syscall(CPUArchState *cpu_env, int num,
                               abi_long ret,
                               abi_long arg1, abi_long arg2, abi_long arg3,
                               abi_long arg4, abi_long arg5, abi_long arg6,
                               abi_long arg7, abi_long arg8)
{
    if (!g_tinyhook_enabled || !g_post_syscall_hooks) {
        return ret;
    }

    PyObject *key = PyLong_FromLong(num);
    if (!key) {
        PyErr_Clear();
        return ret;
    }

    PyObject *callback = PyDict_GetItem(g_post_syscall_hooks, key);
    Py_DECREF(key);

    if (!callback) {
        return ret;
    }

    /* Call the Python callback */
    PyObject *py_result = PyObject_CallFunction(callback, "iLLLLLLLLL",
                                                num,
                                                (long long)ret,
                                                (long long)arg1,
                                                (long long)arg2,
                                                (long long)arg3,
                                                (long long)arg4,
                                                (long long)arg5,
                                                (long long)arg6,
                                                (long long)arg7,
                                                (long long)arg8);
    if (!py_result) {
        fprintf(stderr, "tinyhook: error in post-syscall hook for syscall %d:\n", num);
        PyErr_Print();
        return ret;
    }

    /* Parse the result - expect an integer return value */
    abi_long new_ret = ret;
    if (PyLong_Check(py_result)) {
        new_ret = (abi_long)PyLong_AsLongLong(py_result);
    }

    Py_DECREF(py_result);
    return new_ret;
}

#else /* !CONFIG_TINYHOOK */

int tinyhook_init(const char *script_path)
{
    fprintf(stderr, "tinyhook: not compiled with Python support\n");
    return -1;
}

void tinyhook_shutdown(void)
{
}

bool tinyhook_enabled(void)
{
    return false;
}

bool tinyhook_pre_syscall(CPUArchState *cpu_env, int num,
                          abi_long arg1, abi_long arg2, abi_long arg3,
                          abi_long arg4, abi_long arg5, abi_long arg6,
                          abi_long arg7, abi_long arg8,
                          TinyHookResult *result)
{
    return false;
}

abi_long tinyhook_post_syscall(CPUArchState *cpu_env, int num,
                               abi_long ret,
                               abi_long arg1, abi_long arg2, abi_long arg3,
                               abi_long arg4, abi_long arg5, abi_long arg6,
                               abi_long arg7, abi_long arg8)
{
    return ret;
}

#endif /* CONFIG_TINYHOOK */
