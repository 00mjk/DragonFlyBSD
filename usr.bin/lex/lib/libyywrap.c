/* libyywrap - flex run-time support library "yywrap" function 
 *
 * $Header: /home/daffy/u0/vern/flex/RCS/libyywrap.c,v 1.1 93/10/02 15:23:09 vern Exp $
 * $FreeBSD: src/usr.bin/lex/lib/libyywrap.c,v 1.4 1999/10/27 07:56:49 obrien Exp $
 * $DragonFly: src/usr.bin/lex/lib/libyywrap.c,v 1.3 2008/04/05 22:10:14 swildner Exp $
 */

int yywrap(void)
{
	return 1;
}
