#!/bin/bash 

# make sure there are at least 2 arguments
if [ -z "$3" ]
then
   echo "Usage: `basename $0` <dir to put conf files in> <mcast addr> <sha1 key> <space seperated list of nodes>"
   exit 1
fi

# save name of output dir
OUTDIR=$1
MCAST=$2
SHA1=$3

# setup authkeys
echo auth 1 > ${OUTDIR}/authkeys
echo 1 sha1 $SHA1 >> ${OUTDIR}/authkeys

# set permissions on authkeys
chmod 600 ${OUTDIR}/authkeys
if [ $? != "0" ]
then
   exit 1
fi

# put mcast information in the middle (ordering is important)
echo "logfacility user" > ${OUTDIR}/ha.cf
echo "mcast eth0 ${MCAST} 3335 1 0" >> ${OUTDIR}/ha.cf
echo "auto_failback off" >> ${OUTDIR}/ha.cf
echo "use_logd no" >> ${OUTDIR}/ha.cf
echo "respawn hacluster /usr/lib/heartbeat/cibmon -d" >> ${OUTDIR}/ha.cf
echo "crm yes" >> ${OUTDIR}/ha.cf

# shift arguments down
shift
shift
shift

until [ -z "$1" ]  # Until all parameters used up...
do
   # append node directive to conf file
   echo "node $1" >> ${OUTDIR}/ha.cf
   shift
done

exit 0