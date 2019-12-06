from psana.event import Event
from psana import dgram
from psana.psexp.packet_footer import PacketFooter
import numpy as np
import os

class TransitionId(object):
    ClearReadout=0
    Reset       =1
    Configure   =2
    Unconfigure =3
    BeginRun    =4
    EndRun      =5
    BeginStep   =6
    EndStep     =7
    Enable      =8
    Disable     =9
    SlowUpdate  =10
    Unused_11   =11
    L1Accept    =12 
    NumberOf    =13
    

class EventManager(object):
    """ Return an event from the received smalldata memoryview (view)

    1) If dm is empty (no bigdata), yield this smd event
    2) If dm is not empty, 
        - with filter fn, fetch one bigdata and yield it.
        - w/o filter fn, fetch one big chunk of bigdata and
          replace smalldata view with the read out bigdata.
          Yield one bigdata event.
    """
    def __init__(self, view, smd_configs, dm, filter_fn=0):
        if view:
            if view == bytearray(b'wait'): # RunParallel (unused bigdata nodes get this wait msg)
                self.smd_events = None
                self.n_events = 0
            else:
                pf = PacketFooter(view=view)
                self.smd_events = pf.split_packets()
                self.n_events = pf.n_packets
        else:
            self.smd_events = None
            self.n_events = 0

        self.smd_configs = smd_configs
        self.dm = dm
        self.n_smd_files = len(self.smd_configs)
        self.filter_fn = filter_fn
        self.cn_events = 0

        if not self.filter_fn and len(self.dm.xtc_files) > 0:
            self._read_bigdata_in_chunk()
            
    def _read_bigdata_in_chunk(self):
        """ Read bigdata chunks of 'size' bytes and store them in views
        Note that views here contain bigdata (and not smd) events.
        All non L1 dgrams are copied from smd_events and prepend
        directly to bigdata chunks.
        """
        self.bigdata = []
        for i in range(self.n_smd_files):
            self.bigdata.append(bytearray())
        
        offsets = [0] * self.n_smd_files
        sizes = [0] * self.n_smd_files
        self.ofsz_batch = np.zeros((self.n_events, self.n_smd_files, 2), dtype=np.intp)
        
        # Look for first L1 event - copy all non L1 to bigdata buffers
        first_L1_pos = -1
        for i, event_bytes in enumerate(self.smd_events):
            if event_bytes:
                smd_evt = Event._from_bytes(self.smd_configs, event_bytes, run=self.dm.run())
                if smd_evt._dgrams[0].service() == TransitionId.L1Accept:
                    ofsz = np.asarray([[d.smdinfo[0].offsetAlg.intOffset, \
                            d.smdinfo[0].offsetAlg.intDgramSize] for d in smd_evt._dgrams])
                    offsets = ofsz[:,0]
                    first_L1_pos = i
                    break
                else:
                    ofsz = np.asarray([[0, memoryview(d).nbytes] for d in smd_evt._dgrams])
                    for smd_id, d in enumerate(smd_evt._dgrams):
                        self.bigdata[smd_id].extend(d)

                if i > 0:
                    self.ofsz_batch[i,:,0] = self.ofsz_batch[i-1,:,0] + self.ofsz_batch[i-1,:,1]
                self.ofsz_batch[i,:,1] = ofsz[:,1]
                
        if first_L1_pos == -1: return

        for i, event_bytes in enumerate(self.smd_events[first_L1_pos:]):
            j = i + first_L1_pos
            if event_bytes:
                smd_evt = Event._from_bytes(self.smd_configs, event_bytes, run=self.dm.run())
                ofsz = np.asarray([[d.smdinfo[0].offsetAlg.intOffset, \
                        d.smdinfo[0].offsetAlg.intDgramSize] for d in smd_evt._dgrams])

                if j > 0:
                    self.ofsz_batch[j,:,0] = self.ofsz_batch[j-1,:,0] + self.ofsz_batch[j-1,:,1]
                self.ofsz_batch[j,:,1] = ofsz[:,1]

                sizes += ofsz[:,1]
        
        for i in range(self.n_smd_files):
            # If no data were filtered, we can assume that all bigdata
            # dgrams starting from the first offset are stored consecutively
            # in the file. We read a chunk of sum(all dgram sizes) and
            # store in a view.
            os.lseek(self.dm.fds[i], offsets[i], 0)
            self.bigdata[i].extend(os.read(self.dm.fds[i], sizes[i]))
            
    def __iter__(self):
        return self

    def __next__(self):
        if self.cn_events == self.n_events: 
            raise StopIteration
        if len(self.dm.xtc_files) == 0:
            smd_evt = Event._from_bytes(self.smd_configs, self.smd_events[self.cn_events], run=self.dm.run())
            self.cn_events += 1
            return smd_evt
        
        if self.filter_fn:
            smd_evt = Event._from_bytes(self.smd_configs, self.smd_events[self.cn_events], run=self.dm.run())
            self.cn_events += 1
            ofsz = np.asarray([[d.smdinfo[0].offsetAlg.intOffset, \
                    d.smdinfo[0].offsetAlg.intDgramSize] for d in smd_evt])
            bd_evt = self.dm.jump(ofsz[:,0], ofsz[:,1])
            return bd_evt
        
        dgrams = [None] * self.n_smd_files
        ofsz = self.ofsz_batch[self.cn_events,:,:]
        for j in range(self.n_smd_files):
            if ofsz[j,1]:
                dgrams[j] = dgram.Dgram(view=self.bigdata[j], config=self.dm.configs[j], offset=ofsz[j,0])
        bd_evt = Event(dgrams, run=self.dm.run())
        self.cn_events += 1
        return bd_evt
        
