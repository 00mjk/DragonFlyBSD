#!/usr/local/www/cgi-bin/tablecg
#
# $DragonFly: site/data/Attic/index.cgi,v 1.3 2006/04/06 12:51:43 justin Exp $

$TITLE(The DragonFly BSD Project)

<table border="0" width="100%" bgcolor="white">
<tr><td align="center">
	<h1>Ongoing DragonFly News</h1></td></tr>
<tr><td>
	<p>The <a href="http://www.shiningsilence.com/dbsdlog/">DragonFly Digest</a>
    has up to date information on recent events and changes.</p>

	<p>The <a href="http://wiki.dragonflybsd.org/">DragonFlyBSD Wiki</a>
	was started by Devon H. O'Dell and Sascha Wildner for things related
	to DragonFly.</p>
	<p>The <a href="http://gobsd.com">GoBSD Community</a> site has information about sub-projects such as adding DragonFly support into Pkgsrc and other developments to help make DragonFly a good general-purpose operating system.</p>
</td></tr>
</table>

<table border="0" width="100%" bgcolor="white">
<tr><td align="center">
    <h1>DragonFly-1.4.x RELEASED!</h1>
	<h1>07 January 2006</h1>
</td></tr></table>
<p>
The <a href="/community/release1_4.cgi"><b>DragonFly-1.4.x Release</b></a> is ready!
</p>

<h1>What is DragonFly BSD?</h1>
<p>
DragonFly is an operating system and environment designed to be the logical
continuation of the FreeBSD-4.x OS series.  These operating systems belong
in the same class as Linux in that they are based on UNIX ideals and APIs.
DragonFly is a fork in the path, so to speak, giving the BSD base an opportunity
to grow in an entirely new direction from the one taken in the FreeBSD-5 series.</p>
<p>
It is our belief that the correct choice of features and algorithms can
yield the potential for excellent scalability,
robustness, and debuggability in a number of broad system categories.  Not
just for SMP or NUMA, but for everything from a single-node UP system to a
massively clustered system.  It is our belief that a fairly simple
but wide-ranging set of goals will lay the groundwork for future growth.
The existing BSD cores, including FreeBSD-5, are still primarily based on
models which could at best be called 'strained' as they are applied to modern
systems.  The true innovation has given way to basically just laying on hacks
to add features, such as encrypted disks and security layering that in a
better environment could be developed at far less cost and with far greater
flexibility.</p>
<p>
We also believe that it is important to provide API solutions which
allow reasonable backwards and forwards version compatibility, at least
between userland and the kernel, in a mix-and-match environment.  If one
considers the situation from the ultimate in clustering... secure anonymous
system clustering over the internet, the necessity of having properly 
specified APIs becomes apparent.</p>
<p>
Finally, we believe that a fully integrated and feature-full upgrade
mechanism should exist to allow end users and system operators of all
walks of life to easily maintain their systems.  Debian Linux has shown us
the way, but it is possible to do better.</p>
<p>
DragonFly is going to be a multi-year project at the very least.  Achieving
our goal set will require a great deal of groundwork just to reposition
existing mechanisms to fit the new models.  The goals link will take you
to a more detailed description of what we hope to accomplish.</p>
