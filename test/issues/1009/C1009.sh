#!/bin/sh
BOOTPARAM="-c 1-7,17-23,9-15,25-31 -m 10G@0,10G@1"
USELTP=1
USEOSTEST=

################################################################################
BINDIR=
SBINDIR=
OSTESTDIR=
LTPDIR=
LTPBIN=
MCEXEC=
TESTMCK=

if [ -f $HOME/mck_test_config ]; then
	. $HOME/mck_test_config
elif [ -f ../../../mck_test_config.sample ]; then
	. ../../../mck_test_config.sample
else
	BIN=
	SBIN=
	OSTEST=
	LTP=
fi

#-------------------------------------------------------------------------------
if [ "x$BIN" = x ]; then
	if [ -f ../../../config.h ]; then
		str=`grep "^#define BINDIR " ../../../config.h | head -1 | sed 's/^#define BINDIR /BINDIR=/'`
		eval $str
	fi
else
	BINDIR="$BIN"
fi

if [ "x$SBIN" = x ]; then
	if [ -f ../../../Makefile ]; then
		str=`grep ^SBINDIR ../../../Makefile | head -1 | sed 's/ //g'`
		eval $str
	fi
else
	SBINDIR="$SBIN"
fi

if [ ! -x "$BINDIR/mcexec" ]; then
	echo no mckernel found $BINDIR >&2
	exit 1
fi
MCEXEC="$BINDIR/mcexec"

#-------------------------------------------------------------------------------
if [ "x$USELTP" != x ]; then
	if [ "x$LTP" = x ]; then
		if [ -f "$HOME/ltp/testcases/bin/fork01" ]; then
			LTPDIR="$HOME/ltp"
		fi
	else
		LTPDIR="$LTP"
	fi

	if [ ! -x "$LTPDIR/testcases/bin/fork01" ]; then
		echo no LTP found $LTPDIR >&2
		exit 1
	fi
	LTPBIN="$LTPDIR/testcases/bin"
fi

#-------------------------------------------------------------------------------
if [ "x$USEOSTEST" != x ]; then
	if [ "x$OSTEST" = x ]; then
		if [ -f "$HOME/ostest/bin/test_mck" ]; then
			OSTESTDIR="$HOME/ostest"
		fi
	else
		OSTESTDIR="$OSTEST"
	fi

	if [ ! -x "$OSTESTDIR"/bin/test_mck ]; then
		echo no ostest found $OSTESTDIR >&2
		exit 1
	fi
	TESTMCK="$OSTESTDIR/bin/test_mck"
fi

#===============================================================================
if [ ! -x "$SBINDIR/mcstop+release.sh" ]; then
	echo mcstop+release: not found >&2
	exit 1
fi
echo -n "mcstop+release.sh ... "
sudo "$SBINDIR/mcstop+release.sh"
echo "done"

if lsmod | grep mcctrl > /dev/null 2>&1; then
	echo mckernel shutdown failed >&2
	exit 1
fi

if [ ! -x "$SBINDIR/mcreboot.sh" ]; then
	echo mcreboot: not found >&2
	exit 1
fi
echo -n "mcreboot.sh $BOOTPARAM ... "
sudo "$SBINDIR/mcreboot.sh" $BOOTPARAM
echo "done"

if ! lsmod | grep mcctrl > /dev/null 2>&1; then
	echo mckernel boot failed >&2
	exit 1
fi

################################################################################
$MCEXEC ./C1009T01

if [ x$LTPDIR = x ]; then
	echo no LTP found >&2
	exit 1
fi

for i in kill01:02 kill12:03 pause02:04 sigaction01:05 ; do
	tp=`echo $i|sed 's/:.*//'`
	id=`echo $i|sed 's/.*://'`
	$MCEXEC $LTPBIN/$tp 2>&1 | tee $tp.txt
	ok=`grep TPASS $tp.txt | wc -l`
	ng=`grep TFAIL $tp.txt | wc -l`
	if [ $ng = 0 ]; then
		echo "*** C1009T$id: $tp OK ($ok)"
	else
		echo "*** C1009T$id: $tp NG (ok=$ok ng=%ng)"
	fi
done