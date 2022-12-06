unset LD_LIBRARY_PATH
unset PYTHONPATH

# these three lines should be customized for the site
source /cds/sw/ds/ana/conda2-v2/inst/etc/profile.d/conda.sh
export CONDA_ENVS_DIRS=/cds/sw/ds/ana/conda2/inst/envs/
conda activate ps-4.5.23

RELDIR="$( cd "$( dirname "${readlink -f BASH_SOURCE[0]}" )" && pwd )"
export PATH=$RELDIR/install/bin:${PATH}
pyver=$(python -c "import sys; print(str(sys.version_info.major)+'.'+str(sys.version_info.minor))")
export PYTHONPATH=$RELDIR/install/lib/python$pyver/site-packages
# for procmgr
export TESTRELDIR=$RELDIR/install
export PROCMGR_EXPORT=RDMAV_FORK_SAFE=1,RDMAV_HUGEPAGES_SAFE=1  # See fi_verbs man page regarding fork()

# needed by Ric to get correct libfabric man pages
export MANPATH=$CONDA_PREFIX/share/man${MANPATH:+:${MANPATH}}
