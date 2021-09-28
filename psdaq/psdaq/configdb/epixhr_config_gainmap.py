from psdaq.configdb.typed_json import cdict
import psdaq.configdb.configdb as cdb
import pyrogue as pr
import numpy as np
import sys
import IPython
import argparse

elemRows = 144
elemCols = 192

def copyValues(din,dout,k=None):
    if isinstance(din,dict) and isinstance(dout[k],dict):
        for key,value in din.items():
            if key in dout[k]:
                copyValues(value,dout[k],key)
            else:
                print(f'skip {key}')
    elif isinstance(din,bool):
        vin = 1 if din else 0
        if dout[k] != vin:
            print(f'Writing {k} = {vin}')
            dout[k] = 1 if din else 0
        else:
            print(f'{k} unchanged')
    else:
        if dout[k] != din:
            print(f'Writing {k} = {din}')
            dout[k] = din
        else:
            print(f'{k} unchanged')

if __name__ == "__main__":

    create = False
    dbname = 'configDB'     #this is the name of the database running on the server.  Only client care about this name.

    args = cdb.createArgs().args

    db = 'configdb' if args.prod else 'devconfigdb'
    mycdb = cdb.configdb(f'https://pswww.slac.stanford.edu/ws-auth/{db}/ws/', args.inst, create,
                         root=dbname, user=args.user, password=args.password)
    top = mycdb.get_configuration(args.alias, args.name+'_%d'%args.segm)

    top['user']['gain_mode'] = 5  # Map
    map = np.ones((4,144,192))*0x8 # low gain
    med = np.ones((64,64),dtype=np.uint8)*0xc
    for i in range(4):
        map[i,:64,:64] = med
        top['expert']['EpixHR'][f'Hr10kTAsic{i}']['trbit'] = 0
    top['user']['pixel_map'] = map

    mycdb.modify_device(args.alias, top)
