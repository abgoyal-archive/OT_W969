#! /bin/sh
die() {
    echo "$*" 1>&2
    exit 1
}

MANDIR=$1
if test x"$MANDIR" = x ; then
    MANDIR=doc/api/man/man3dbus
fi

cd "$MANDIR" || die "Could not cd to $MANDIR"

test -d keep || mkdir keep || die "Could not create $MANDIR/keep directory"
test -d nuke || mkdir nuke || die "Could not create $MANDIR/nuke directory"

## blacklist
(find . -maxdepth 1 -name "_*" | xargs -I ITEMS /bin/mv ITEMS nuke) || die "could not move all underscore-prefixed items"
(find . -maxdepth 1 -name "DBus*Internal*" | xargs -I ITEMS /bin/mv ITEMS nuke) || die "could not move all internal-containing items"
(find . -maxdepth 1 -name "dbus_*_internal_*" | xargs -I ITEMS /bin/mv ITEMS nuke) || die "could not move all internal-containing items"

## this is kind of unmaintainable, but I guess it's no huge disaster if we miss something.
## this list should only contain man pages with >1 line, i.e. with real content; the 
## one-line cross-references get cleaned up next.
for I in DBusCounter.* DBusCredentials.* DBusDataSlot.* DBusDataSlotAllocator.* DBusDataSlotList.* \
    DBusDirIter.* DBusFakeMutex.* DBusFreedElement.* DBusGroupInfo.* DBusGUID.* DBusHashEntry.* \
    DBusHashIter.* DBusHashTable.* DBusHeader.* DBusHeaderField.* DBusKey.* DBusKeyring.* DBusList.* \
    DBusMarshal.* DBusMD5* DBusMemBlock.* DBusMemPool.* DBusMessageGenerator.* DBusMessageLoader.* \
    DBusMessageRealIter.* DBusObjectSubtree.* DBusObjectTree.* DBusPollFD.* DBusReal* \
    DBusResources.* DBusServerDebugPipe.* DBusServerSocket.* DBusServerUnix.* \
    DBusServerVTable.* DBusSHA.* DBusSHAContext.* DBusSignatureRealIter.* DBusStat.* DBusString.* \
    DBusSysdeps.* DBusSysdepsUnix.* DBusTimeoutList.* DBusTransport* DBusTypeReader* DBusTypeWriter* \
    DBusUserInfo.* DBusWatchList.* ; do 
    if test -f "$I" ; then
        /bin/mv "$I" nuke || die "could not move $I to $MANDIR/nuke"
    fi
done

## many files just contain ".so man3dbus/DBusStringInternals.3dbus" or the like, 
## if these point to something we nuked, we want to also nuke the pointer.
for I in * ; do
    if test -f "$I" ; then
        LINES=`wc -l "$I" | cut -d ' ' -f 1`
        if test x"$LINES" = x1 ; then
            REF_TO=`cat "$I" | sed -e 's/\.so man3dbus\///g'`
            ## echo "$I points to $REF_TO"
            if ! test -f "$REF_TO" ; then
                /bin/mv "$I" nuke || die "could not move $I to $MANDIR/nuke"
            fi
        fi
    fi
done

## whitelist
(find . -maxdepth 1 -name "dbus_*" | xargs -I ITEMS /bin/mv ITEMS keep) || die "could not move all dbus-prefixed items"
(find . -maxdepth 1 -name "DBUS_*" | xargs -I ITEMS /bin/mv ITEMS keep) || die "could not move all DBUS_-prefixed items"
(find . -maxdepth 1 -name "DBus*" | xargs -I ITEMS /bin/mv ITEMS keep) || die "could not move all DBus-prefixed items"

## everything else is assumed irrelevant, this is mostly struct fields
## from the public headers
(find . -maxdepth 1 -type f | xargs -I ITEMS /bin/mv ITEMS nuke) || die "could not move remaining items"

NUKE_COUNT=`find nuke -type f -name "*" | wc -l`
KEEP_COUNT=`find keep -type f -name "*" | wc -l`
MISSING_COUNT=`find . -maxdepth 1 -type f -name "*" | wc -l`

echo "$KEEP_COUNT man pages kept and $NUKE_COUNT man pages to remove"
echo "$MISSING_COUNT not handled"

(find keep -type f -name "*" | xargs -I ITEMS /bin/mv ITEMS .) || die "could not move kept items back"

rmdir keep || die "could not remove $MANDIR/keep"

echo "Man pages to be installed are in $MANDIR and man pages to ignore are in $MANDIR/nuke"

exit 0
