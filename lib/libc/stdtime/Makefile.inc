#	Makefile.inc,v 1.2 1994/09/13 21:26:01 wollman Exp
# $FreeBSD: head/lib/libc/stdtime/Makefile.inc 248672 2013-03-24 01:35:37Z dim $

.PATH:	${.CURDIR}/../libc/stdtime

CMAPS+=	${.CURDIR}/stdtime/Symbol.map

SRCS+=	asctime.c difftime.c getdate.c localtime.c \
	strftime.c strptime.c timelocal.c time32.c timespec_get.c

.if ${LIB} == "c"
MAN+=	ctime.3 getdate.3 offtime.3 strftime.3 strptime.3 \
	time2posix.3 timespec_get.3
MAN+=	tzfile.5

MLINKS+=ctime.3 asctime.3 \
	ctime.3 asctime_r.3 \
	ctime.3 ctime_r.3 \
	ctime.3 difftime.3 \
	ctime.3 gmtime.3 \
	ctime.3 gmtime_r.3 \
	ctime.3 localtime.3 \
	ctime.3 localtime_r.3 \
	ctime.3 mktime.3 \
	ctime.3 timegm.3
MLINKS+=offtime.3 timelocal.3 \
	offtime.3 timeoff.3
MLINKS+=strftime.3 strftime_l.3
MLINKS+=strptime.3 strptime_l.3
MLINKS+=time2posix.3 posix2time.3
.endif
