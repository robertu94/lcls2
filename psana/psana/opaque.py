import hsd as Hsd

def OpaqueRawData(name, config):
    sw = getattr(config, 'software')
    det = getattr(sw, name)
    df = eval(str(det.dettype) + '._Factory()')
    return df.create(name, config)

class OpaqueRawDataBase(object):
    def __init__(self, name, config):
        self.detectorName = name
        self.config = config

class hsd(OpaqueRawDataBase):
    """
    hsd reader
    """
    def __init__(self, name, config):
        super(hsd, self).__init__(name, config)
        self.name = name
        sw = getattr(config, 'software')
        detcfg = getattr(sw, name)
        self.nchan = getattr(config, 'nchan')
        assert detcfg.dettype == 'hsd'
        assert detcfg.hsd.software == 'hsd'

    def __call__(self, evt):
        # FIXME: discover how many channels there are
        # FIXME: find out 0 in dgrams[0]
        #chan0 = evt.dgrams[0].xpphsd.hsd.chan0
        #chan1 = evt.dgrams[0].xpphsd.hsd.chan1
        #chan2 = evt.dgrams[0].xpphsd.hsd.chan2
        #chan3 = evt.dgrams[0].xpphsd.hsd.chan3
        #chans = [chan0, chan1, chan2, chan3]

        chans = []
        for i in range(self.nchan):
            chans.append( eval('evt.dgrams[0].xpphsd.hsd.chan'+str(i)) )
        nonOpaqueHsd = Hsd.hsd("1.0.0", chans)  # make an object per event
        return nonOpaqueHsd

    class _Factory:
        def create(self, name, config): return hsd(name, config)
