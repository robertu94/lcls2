from libc.string    cimport memcpy
from libc.stdlib    cimport malloc, free
from libcpp.string  cimport string
from cpython.buffer cimport PyObject_GetBuffer, PyBuffer_Release, PyBUF_ANY_CONTIGUOUS, PyBUF_SIMPLE
from psana.dgrampy  cimport *
from cpython cimport array
import array
import numpy as np
import numbers

# Need a different name to prevent clashing with cpp class
from psana.psexp import TransitionId as PyTransitionId 


class AlgDef:
    def __init__(self, name, major, minor, micro):
        self.name   = name
        self.major  = major
        self.minor  = minor
        self.micro  = micro

class DetectorDef:
    def __init__(self, name, dettype, detid):
        self.name       = name
        self.dettype    = dettype
        self.detid      = detid

class NamesDef:
    def __init__(self, nodeId, namesId):
        self.nodeId = nodeId
        self.namesId = namesId


class DataType:
    """ This list has to match Name.DataType c++
    {UINT8, UINT16, UINT32, UINT64, INT8, INT16, INT32, INT64, FLOAT, DOUBLE,
    CHARSTR, ENUMVAL, ENUMDICT}
    but it doesn't make sense to wrap it ..."""
    nptypes = {
                0: np.uint8,
                1: np.uint16,
                2: np.uint32,
                3: np.uint64,
                4: np.int8,
                5: np.int16,
                6: np.int32,
                7: np.int64,
                8: np.float32,
                9: np.float64,
                10: str,
    }
    psana_types = {np.uint8:    0,
                   np.uint16:   1,
                   np.uint32:   2,
                   np.uint64:   3,
                   np.int8:     4,
                   np.int16:    5,
                   np.int32:    6,
                   np.int64:    7,
                   np.float32:  8,
                   np.float64:  9,
                   str:         10,
                  }
    def to_psana2(self, numpy_dtype):
        return self.psana_types[numpy_dtype]

    def to_numpy(self, psana_dtype):
        return self.nptypes[psana_dtype]

cdef class PyDataDef:
    cdef DataDef* cptr

    def __init__(self):
        self.cptr = new DataDef()

    def show(self):
        self.cptr.show()

    def add(self, name, dtype, rank):
        self.cptr.add(name.encode(), dtype, rank)

    def get_dtype(self, name):
        return self.cptr.getDtype(name.encode())

    def get_rank(self, name):
        return self.cptr.getRank(name.encode())

    def is_number(self, name):
        flag = False
        if self.cptr.getDtype(name.encode()) != DataType.psana_types[str]:
            flag = True
        return flag

cdef class PyXtc():
    cdef Xtc* cptr

    # No constructor - the Xtc ptr gets assigned elsewhere.

    def sizeofPayload(self):
        return self.cptr.sizeofPayload()

cdef class PyDgram():
    # TODO: Set these two attributes via constructor.
    cdef Dgram* cptr

    # Limit identifier for the dgram. We use size() from XtcFileiterator
    # to identify the end of the buffer in case of reading it from a file.
    cdef void* bufEnd           

    # No constructor - the Dgram ptr gets assigned elsewhere.

    def get_pyxtc(self):
        pyxtc = PyXtc()
        pyxtc.cptr = &(self.cptr.xtc)
        return pyxtc

    def get_size(self):
        return sizeof(Dgram)

    def dec_extent(self, uint32_t removed_size):
        self.cptr.xtc.extent -= removed_size

    def get_payload_size(self):
        return self.cptr.xtc.sizeofPayload()

    def service(self):
        return self.cptr.service()


cdef class PyXtcUpdateIter():
    cdef XtcUpdateIter* cptr
    _numWords = 6               # no. of print-out elements for an array

    def __cinit__(self):
        self.cptr = new XtcUpdateIter(self._numWords)

    def savextc2(self, f):
        """Writes data from _buf to xtc2 file and resets the buffer.
        """
        f.write(self.get_buf())
        self.cptr.clear_buf()

    def process(self, PyXtc pyxtc):
        self.cptr.process(pyxtc.cptr)

    def iterate(self, PyXtc pyxtc):
        self.cptr.iterate(pyxtc.cptr)

    def get_buf(self):
        cdef char[:] buf
        cdef uint64_t bufsize=0

        bufsize = self.cptr.get_bufsize()
        if bufsize > 0:
            buf = <char [:bufsize]>self.cptr.get_buf()
            return buf
        else:
            return memoryview(bytearray())

    def copy(self, PyDgram pydg, is_config=False):
        self.cptr.copy(pydg.cptr, is_config)

    def updatetimestamp(self, PyDgram pydg, timestamp_val):
        self.cptr.updateTimeStamp(pydg.cptr[0], timestamp_val)

    def names(self, PyDgram pydg, detdef, algdef, PyDataDef pydatadef,
            nodeId, namesId, segment):
        cdef PyXtc pyxtc = pydg.get_pyxtc()
        
        # Passing string to c needs utf-8 encoding
        detName = detdef.name.encode()
        detType = detdef.dettype.encode()
        detId   = detdef.detid.encode()
        algName = algdef.name.encode()

        # For automatic assignment, 
        # for namesId, increment the current value by 1.
        # for nodeId, use the current nodeId of this Configure.
        # for segment, use 0.
        if nodeId is None: nodeId = self.cptr.getNodeId()
        if namesId is None: namesId = self.cptr.getMaxUsedNamesId() + 1
        if segment is None: segment = 0

        # Dereference in cython with * is not allowed. You can either use [0] index or
        # from cython.operator import dereference
        # cdef Xtc xtc = dereference(pyxtc.cptr)
        self.cptr.addNames(pyxtc.cptr[0], pydg.bufEnd, detName, detType, detId,
                nodeId, namesId, segment,
                algName, algdef.major, algdef.minor, algdef.micro,
                pydatadef.cptr[0])
        return NamesDef(nodeId, namesId)

    def createdata(self, PyDgram pydg, namesdef):
        cdef PyXtc pyxtc = pydg.get_pyxtc()
        self.cptr.createData(pyxtc.cptr[0], pydg.bufEnd, namesdef.nodeId, namesdef.namesId)

    def setstring(self, PyDataDef pydatadef, datadef_name, str_data):
        self.cptr.setString(str_data.encode(), pydatadef.cptr[0], datadef_name.encode())

    def setvalue(self, namesdef, PyDataDef pydatadef, datadef_name, data):
        """Sets scalar value to the given `datadef_name`.

        Converts (any) given scalar `data` to the specified known dtype
        using numpy array format. The buffer is passed in (and later converted
        to the correct dtype) as char*.
        """
        cdef int dtypeNo = pydatadef.cptr.getDtype(datadef_name.encode())

        d_arr = np.array([data], dtype=DataType.nptypes[dtypeNo])
        cdef char* data_ptr
        cdef Py_buffer data_pybuf
        PyObject_GetBuffer(d_arr, &data_pybuf, PyBUF_SIMPLE | PyBUF_ANY_CONTIGUOUS)
        data_ptr = <char *>data_pybuf.buf
        self.cptr.setValue(namesdef.nodeId, namesdef.namesId,
                           data_ptr, pydatadef.cptr[0], datadef_name.encode())
        PyBuffer_Release(&data_pybuf)

    def adddata(self, namesdef, PyDataDef pydatadef,
            datadef_name, unsigned[:] shape, data):
        cdef unsigned* shape_ptr
        cdef Py_buffer shape_pybuf
        PyObject_GetBuffer(shape, &shape_pybuf, PyBUF_SIMPLE | PyBUF_ANY_CONTIGUOUS)
        shape_ptr = <unsigned *>shape_pybuf.buf

        cdef char* data_ptr
        cdef Py_buffer data_pybuf
        PyObject_GetBuffer(data, &data_pybuf, PyBUF_SIMPLE | PyBUF_ANY_CONTIGUOUS)
        data_ptr = <char *>data_pybuf.buf

        self.cptr.addData(namesdef.nodeId, namesdef.namesId, shape_ptr,
                data_ptr, pydatadef.cptr[0], datadef_name.encode())

        PyBuffer_Release(&shape_pybuf)
        PyBuffer_Release(&data_pybuf)

    def set_filter(self, det_name, alg_name):
        self.cptr.setFilter(det_name.encode(), alg_name.encode())

    def clear_filter(self):
        self.cptr.clearFilter()
        self.cptr.resetRemovedSize()

    def createTransition(self, transId, timestamp_val):
        counting_timestamps = 1
        if timestamp_val == -1:
            counting_timestamps = 0
            timestamp_val = 0

        pydg = PyDgram()
        # This returns Dgram and updates bufEnd to point to 
        # buffer size of the dgram (defined in XtcUpdateIter).
        pydg.cptr = &(self.cptr.createTransition(transId,
                counting_timestamps, timestamp_val, &(pydg.bufEnd)))
        return pydg

    def get_removed_size(self):
        return self.cptr.get_removed_size()

    def set_cfgwrite(self, int flag):
        self.cptr.setCfgWriteFlag(flag)
    
    def set_cfg(self, int flag):
        self.cptr.setCfgFlag(flag)

    def is_config(self):
        return self.cptr.isConfig()


cdef class PyXtcFileIterator():
    cdef XtcFileIterator* cptr

    def __cinit__(self, int fd, size_t maxDgramSize):
        self.cptr = new XtcFileIterator(fd, maxDgramSize)

    def next(self):
        cdef Dgram* dg
        dg = self.cptr.next()
        pydg = PyDgram()
        pydg.cptr = dg
        pydg.bufEnd = <char *>dg + self.cptr.size()
        return pydg

class DgramPy:
    """ Main class that takes an existing dgram or creates
    new dgram when `TransitionId` is given and allow modifications
    on the dgram.
    """
    def __init__(self, 
                 PyDgram pydg=None, 
                 config=None,
                 transid=None,
                 ts=-1):
        # We need to have only one PyXtcUpdateIter when working with 
        # the same xtc. This class always read in config first and
        # populates NamesLookup, which is used by other dgrams.
        if pydg:
            if config:
                self.uiter = config.uiter
                self.uiter.set_cfg(False)
            else:
                # Assumes this is a config so we create new XtcUpdateIter here.
                self.uiter = PyXtcUpdateIter()
                self.uiter.set_cfg(True)
                # Iterates Configure without writing to buffer to get
                # current nodeId and namesId
                self.uiter.set_cfgwrite(False)
                self.uiter.iterate(pydg.get_pyxtc())

            self.pydg = pydg
        elif transid:
            if transid == PyTransitionId.Configure:
                self.uiter = PyXtcUpdateIter()
                self.uiter.set_cfg(True)
            else:
                assert config
                self.uiter = config.uiter
                self.uiter.set_cfg(False)
            self.pydg = self.uiter.createTransition(transid, ts)
        else:
            raise IOError, "Unsupported input arguments"

    def Detector(self, detdef, algdef, datadef_dict,
            nodeId=None, namesId=None, segment=None):
        """Returns Names object, which represents to a detector."""
        assert self.uiter.is_config() == 1, "Expect a Configure dgram."
        
        # Creates a datadef from data definition dictionary
        pydatadef = PyDataDef()
        dt = DataType()
        for key, (numpy_dtype, rank)  in datadef_dict.items():
            pydatadef.add(key, dt.to_psana2(numpy_dtype), rank)
        
        det = self.uiter.names(self.pydg, detdef, algdef, pydatadef,
                nodeId, namesId, segment)
        setattr(det, 'datadef', pydatadef)
        
        class Container:
            """Wraps all fields  in `pydatadef`"""
            def __init__(self):
                for dtdef_name, _ in datadef_dict.items():
                    setattr(self, dtdef_name, None)

        setattr(det, algdef.name, Container())
        return det


    def adddata(self, det, data_container):
        cdef array.array shape = array.array('I', [0,0,0,0,0])
        cdef int i

        self.uiter.createdata(self.pydg, det)
        
        pydatadef = det.datadef
        datadict = data_container.__dict__
        dt = DataType()

        for datadef_name, data in datadict.items():
            assert data is not None, f"Missing data for '{datadef_name}'."

            # Check rank (dimension) of data (note that string is rank 1 in psana2).
            data_rank = np.ndim(data)
            if isinstance(data, str): 
                data_rank = 1
            
            # Prepares error messages for incorrect ranks and types
            err_r = f"Incorrect rank for '{datadef_name}'. Expected: {pydatadef.get_rank(datadef_name)}."
            err_t = f"Incorrect type for '{datadef_name}'. Expected: {DataType.nptypes[pydatadef.get_dtype(datadef_name)]}."
            assert data_rank == pydatadef.get_rank(datadef_name), err_r


            # Handle scalar types (string or number)
            if np.ndim(data) == 0:
                if pydatadef.is_number(datadef_name):
                    assert isinstance(data, numbers.Number), err_t
                    self.uiter.setvalue(det, pydatadef, datadef_name, data)
                else:
                    assert isinstance(data, str), err_t
                    self.uiter.setstring(pydatadef, datadef_name, data)
            else:
                # Handle array type
                assert data.dtype.type != np.str_, f"Incorrect data for '{datadef_name}'. Array of string is not permitted. Use string instead (e.g. 'hello world')."
                assert pydatadef.get_dtype(datadef_name) == dt.to_psana2(data.dtype.type), err_t
                
                array.zero(shape)
                for i in range(len(shape)):
                    if i < len(data.shape):
                        shape[i] = data.shape[i]
                self.uiter.adddata(det, pydatadef, datadef_name, shape, data)
            # Clears data container for next event
            datadict[datadef_name] = None


    def removedata(self, det_name, alg_name):
        self.uiter.set_filter(det_name, alg_name)

    def save(self, xtc2file):
        """ 
        For L1Accept,
        Copies ShapesData to _tmpbuf and update
        parent dgram extent to new size (if some ShapesData
        were removed. 
        
        For Configure,
        Calls iterate after setting cfgWriteFlag to True. This 
        does two things:
        1. Iterating Names (writing is optional because we also
        just want to count NodeId and NamesId here). will also 
        save to _cfgbuf
        2. Iterating ShapesData (writing is always) will save to 
        _cfgbuf instead of _tmpbuf (for non-configure dgrams). 
        
        The parent dgram and _tmpbuf/_cfgbuf are then
        copied to _buf as one new event.
        """
        pyxtc = self.pydg.get_pyxtc()
        is_config = False
        self.uiter.set_cfgwrite(False)
        if self.pydg.service() == PyTransitionId.Configure:
            is_config = True
            self.uiter.set_cfgwrite(True)
        
        # Copies Names for Configure or ShapesData for L1Accept 
        # (also applies remove for ShapesData).
        self.uiter.iterate(pyxtc)

        # Update parent dgram with new size
        cdef uint32_t removed_size = self.uiter.get_removed_size()
        if removed_size > 0:
            self.pydg.dec_extent(removed_size)
        
        # Copy updated parent dgram and _tmpbuf/_cfgbuf to _buf then save.
        self.uiter.copy(self.pydg, is_config=is_config)
        self.uiter.savextc2(xtc2file)

        # Clear filter flags and reset removed size for the next event
        self.uiter.clear_filter()

    def updatetimestamp(self, timestamp_val):
        self.uiter.updatetimestamp(self.pydg, timestamp_val)


