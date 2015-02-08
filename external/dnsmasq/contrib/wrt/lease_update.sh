#!/bin/sh
NVRAM=/usr/sbin/nvram
PREFIX=dnsmasq_lease_

# Arguments.
# $1 is action (add, del, old)
# $2 is MAC 
# $3 is address
# $4 is hostname (optional, may be unset)

# env.
# DNSMASQ_LEASE_LENGTH or DNSMASQ_LEASE_EXPIRES (which depends on HAVE_BROKEN_RTC)
# DNSMASQ_CLIENT_ID (optional, may be unset)

# File.
# length|expires MAC addr hostname|* CLID|* 

# Primary key is address.

if [ ${1} = init ] ; then
     ${NVRAM} show | sed -n -e "/^${PREFIX}.*/ s/^.*=//p"
else
     if [ ${1} = del ] ; then
          ${NVRAM} unset ${PREFIX}${3}
     fi

     if [ ${1} = old ] || [ ${1} = add ] ; then
          ${NVRAM} set ${PREFIX}${3}="${DNSMASQ_LEASE_LENGTH:-}${DNSMASQ_LEASE_EXPIRES:-} ${2} ${3} ${4:-*} ${DNSMASQ_CLIENT_ID:-*}"
     fi
     ${NVRAM} commit
fi




 
