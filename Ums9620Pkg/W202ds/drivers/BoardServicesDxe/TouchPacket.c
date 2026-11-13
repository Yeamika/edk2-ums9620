// SPDX-License-Identifier: BSD-2-Clause-Patent
/* HX83102J wire format: ten big-endian XY pairs, pressure/padding, count at
 * byte 52, modulo-256 checksum over 56 bytes. No chip or OS dependencies.
 * -1 invalid, 0 valid release, 1 valid single contact. Multiple contacts are
 * rejected: a boot menu must not turn a second finger into a different click.
 */
int W202ParseTouch(const unsigned char *p, unsigned n,
                  unsigned *x, unsigned *y, unsigned *down) {
  unsigned sum=0, nonzero=0, count, found=0, i, px=0, py=0;
  if (!p || !x || !y || !down || n<56) return -1;
  for (i=0;i<56;++i) { sum+=p[i]; nonzero|=p[i]; }
  if ((sum & 255) || !nonzero) return -1;
  count=p[52]==255 ? 0 : p[52]&15;
  if (count>1 || ((p[52]>>4)!=15 && (p[52]>>4)!=0)) return -1;
  if (!count) { *down=0; return 0; }
  for (i=0;i<10;++i) {
    unsigned a=((unsigned)p[i*4]<<8)|p[i*4+1];
    unsigned b=((unsigned)p[i*4+2]<<8)|p[i*4+3];
    if (a==65535 && b==65535) continue;
    if (a>=1200 || b>=1920) return -1;
    px=a; py=b; ++found;
  }
  if (found!=1) return -1;
  *x=px; *y=py; *down=1;
  return 1;
}
