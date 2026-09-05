/*
 * jpeg2pdf - wrap one baseline JPEG in a single-page PDF.
 *
 * A scanned page is already JPEG by the time it reaches us, and PDF can carry
 * a JPEG bitstream verbatim as a /DCTDecode image stream. So the whole job is
 * to copy the file through and write about twenty lines of PDF around it: no
 * decoding, no re-encoding, no image library.
 *
 * This exists because img2pdf - which does the same thing correctly - costs
 * 5.8s per page on a 700MHz ARMv6, and 4.9s of that is `import img2pdf`
 * pulling in PIL and pikepdf before it has looked at the file. That is dead
 * weight on a machine where the scan itself takes 16s. This does it in
 * milliseconds and drops a Python dependency from the press-to-scan path.
 *
 * Deliberately narrow: one baseline JPEG in, one page out. Anything it does
 * not understand is an error, and the caller falls back to img2pdf.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* JPEG markers we care about. */
#define M_SOI   0xD8    /* start of image */
#define M_APP0  0xE0    /* JFIF, carries pixel density */
#define M_SOS   0xDA    /* start of scan - entropy data follows, stop parsing */
#define M_EOI   0xD9

struct jpeg_info {
	unsigned width, height;
	int      components;    /* 1 = gray, 3 = YCbCr/RGB, 4 = CMYK */
	double   dpi_x, dpi_y;  /* 0 if the file does not say */
};

/*
 * Pull the frame geometry out of a JPEG without decoding it.
 *
 * Walks the marker segments up to SOS. The frame header (SOFn) carries height,
 * width and component count; JFIF's APP0 carries the pixel density when the
 * encoder bothered to set it.
 *
 * SOF0/1 are baseline and extended sequential. SOF2 (progressive) is also
 * legal in PDF but decoded inconsistently by viewers, and scanners do not
 * emit it, so it is rejected rather than silently producing a file that
 * renders differently everywhere. The arithmetic-coded (SOF9-11) and
 * hierarchical (SOF13-15) variants are rejected for the same reason.
 */
static int jpeg_probe(FILE *f, struct jpeg_info *out)
{
	unsigned char b[4];

	memset(out, 0, sizeof *out);

	if (fread(b, 1, 2, f) != 2 || b[0] != 0xFF || b[1] != M_SOI) {
		fprintf(stderr, "jpeg2pdf: not a JPEG (no SOI marker)\n");
		return -1;
	}

	for (;;) {
		int c, marker;
		unsigned seglen;

		/* Markers are 0xFF followed by a type byte; 0xFF is a legal
		 * fill byte between segments, so skip any run of them. */
		do {
			if ((c = fgetc(f)) == EOF)
				goto truncated;
		} while (c != 0xFF);
		do {
			if ((marker = fgetc(f)) == EOF)
				goto truncated;
		} while (marker == 0xFF);

		/* Standalone markers: no length field, nothing we need. */
		if (marker == M_SOI || marker == M_EOI ||
		    (marker >= 0xD0 && marker <= 0xD7))
			continue;

		if (marker == M_SOS)
			break;  /* geometry must have appeared before here */

		if (fread(b, 1, 2, f) != 2)
			goto truncated;
		seglen = (unsigned)b[0] << 8 | b[1];
		if (seglen < 2) {
			fprintf(stderr, "jpeg2pdf: bad segment length %u\n", seglen);
			return -1;
		}

		if (marker >= 0xC0 && marker <= 0xCF &&
		    marker != 0xC4 /* DHT */ &&
		    marker != 0xC8 /* reserved */ &&
		    marker != 0xCC /* DAC */) {
			unsigned char sof[6];

			if (marker != 0xC0 && marker != 0xC1) {
				fprintf(stderr, "jpeg2pdf: unsupported JPEG type "
					"(SOF%d); only baseline is handled\n",
					marker - 0xC0);
				return -1;
			}
			/* precision, height, width, component count */
			if (fread(sof, 1, sizeof sof, f) != sizeof sof)
				goto truncated;
			out->height     = (unsigned)sof[1] << 8 | sof[2];
			out->width      = (unsigned)sof[3] << 8 | sof[4];
			out->components = sof[5];

			if (!out->width || !out->height) {
				fprintf(stderr, "jpeg2pdf: zero-sized image\n");
				return -1;
			}
			if (out->components != 1 && out->components != 3 &&
			    out->components != 4) {
				fprintf(stderr, "jpeg2pdf: %d components is not "
					"a colour space PDF can name\n",
					out->components);
				return -1;
			}
			break;  /* everything needed is in hand */
		}

		if (marker == M_APP0 && seglen >= 16) {
			unsigned char app0[14];

			if (fread(app0, 1, sizeof app0, f) != sizeof app0)
				goto truncated;
			if (!memcmp(app0, "JFIF\0", 5)) {
				unsigned xd = (unsigned)app0[8]  << 8 | app0[9];
				unsigned yd = (unsigned)app0[10] << 8 | app0[11];

				/* units: 1 = per inch, 2 = per cm, 0 = aspect
				 * ratio only, which says nothing about size. */
				if (app0[7] == 1) {
					out->dpi_x = xd;
					out->dpi_y = yd;
				} else if (app0[7] == 2) {
					out->dpi_x = xd * 2.54;
					out->dpi_y = yd * 2.54;
				}
			}
			if (fseek(f, (long)seglen - 2 - (long)sizeof app0, SEEK_CUR))
				goto truncated;
			continue;
		}

		if (fseek(f, (long)seglen - 2, SEEK_CUR))
			goto truncated;
	}

	if (!out->width) {
		fprintf(stderr, "jpeg2pdf: no frame header found\n");
		return -1;
	}
	return 0;

truncated:
	fprintf(stderr, "jpeg2pdf: JPEG ended mid-header\n");
	return -1;
}

static const char *colorspace_for(int components)
{
	switch (components) {
	case 1:  return "/DeviceGray";
	case 4:  return "/DeviceCMYK";
	default: return "/DeviceRGB";
	}
}

int main(int argc, char **argv)
{
	const char *in_path = NULL, *out_path = NULL;
	double dpi_opt = 0;
	FILE *in, *out;
	struct jpeg_info info;
	long jpeg_len, offsets[6];
	double page_w, page_h;
	char imgdict[512], content[128];
	unsigned char buf[65536];
	size_t n, copied = 0;
	long startxref;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc) {
			out_path = argv[++i];
		} else if (!strcmp(argv[i], "--dpi") && i + 1 < argc) {
			dpi_opt = atof(argv[++i]);
			if (dpi_opt <= 0) {
				fprintf(stderr, "jpeg2pdf: --dpi must be positive\n");
				return 2;
			}
		} else if (argv[i][0] == '-' && argv[i][1]) {
			fprintf(stderr, "jpeg2pdf: unknown option %s\n", argv[i]);
			return 2;
		} else if (!in_path) {
			in_path = argv[i];
		} else {
			fprintf(stderr, "jpeg2pdf: only one input is supported\n");
			return 2;
		}
	}
	if (!in_path || !out_path) {
		fprintf(stderr, "usage: jpeg2pdf [--dpi N] <in.jpg> -o <out.pdf>\n");
		return 2;
	}

	if (!(in = fopen(in_path, "rb"))) {
		fprintf(stderr, "jpeg2pdf: %s: %s\n", in_path, strerror(errno));
		return 1;
	}
	if (jpeg_probe(in, &info)) {
		fclose(in);
		return 1;
	}
	if (fseek(in, 0, SEEK_END) || (jpeg_len = ftell(in)) < 0) {
		fprintf(stderr, "jpeg2pdf: %s: cannot size input\n", in_path);
		fclose(in);
		return 1;
	}
	rewind(in);

	/* Page size in PostScript points (72 per inch). An explicit --dpi wins
	 * over the file's own JFIF density: the caller asked the scanner for a
	 * resolution and knows it, whereas the density field is whatever the
	 * encoder happened to write. Falling back to 72 would be a lie, so if
	 * neither source has a figure, say so rather than emit a page whose
	 * physical size is wrong. */
	{
		double dx = dpi_opt ? dpi_opt : info.dpi_x;
		double dy = dpi_opt ? dpi_opt : info.dpi_y;

		if (dx <= 0 || dy <= 0) {
			fprintf(stderr, "jpeg2pdf: no resolution in the JPEG; "
				"pass --dpi\n");
			fclose(in);
			return 1;
		}
		page_w = info.width  * 72.0 / dx;
		page_h = info.height * 72.0 / dy;
	}

	if (!(out = fopen(out_path, "wb"))) {
		fprintf(stderr, "jpeg2pdf: %s: %s\n", out_path, strerror(errno));
		fclose(in);
		return 1;
	}

	/* The image is drawn by a content stream that scales the unit square to
	 * the page, so the image dictionary carries pixels and the page carries
	 * physical size. */
	snprintf(content, sizeof content, "q %.4f 0 0 %.4f 0 0 cm /Im0 Do Q\n",
		 page_w, page_h);
	snprintf(imgdict, sizeof imgdict,
		 "<</Type/XObject/Subtype/Image/Width %u/Height %u"
		 "/ColorSpace %s/BitsPerComponent 8/Filter/DCTDecode"
		 "/Length %ld>>",
		 info.width, info.height, colorspace_for(info.components),
		 jpeg_len);

#define EMIT(...) do { if (fprintf(out, __VA_ARGS__) < 0) goto wr_err; } while (0)

	EMIT("%%PDF-1.4\n");
	/* A comment of high-bit bytes marks the file as binary, so tools that
	 * sniff content do not mangle it in transit. */
	EMIT("%%\xE2\xE3\xCF\xD3\n");

	offsets[1] = ftell(out);
	EMIT("1 0 obj\n<</Type/Catalog/Pages 2 0 R>>\nendobj\n");

	offsets[2] = ftell(out);
	EMIT("2 0 obj\n<</Type/Pages/Kids[3 0 R]/Count 1>>\nendobj\n");

	offsets[3] = ftell(out);
	EMIT("3 0 obj\n<</Type/Page/Parent 2 0 R/MediaBox[0 0 %.4f %.4f]"
	     "/Resources<</XObject<</Im0 5 0 R>>>>/Contents 4 0 R>>\nendobj\n",
	     page_w, page_h);

	offsets[4] = ftell(out);
	EMIT("4 0 obj\n<</Length %zu>>\nstream\n%sendstream\nendobj\n",
	     strlen(content), content);

	offsets[5] = ftell(out);
	EMIT("5 0 obj\n%s\nstream\n", imgdict);

	while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
		if (fwrite(buf, 1, n, out) != n)
			goto wr_err;
		copied += n;
	}
	if (ferror(in)) {
		fprintf(stderr, "jpeg2pdf: %s: read error\n", in_path);
		goto fail;
	}
	/* /Length was written from the file size; if the copy came up short the
	 * PDF would be structurally wrong in a way readers report vaguely. */
	if (copied != (size_t)jpeg_len) {
		fprintf(stderr, "jpeg2pdf: %s: read %zu of %ld bytes\n",
			in_path, copied, jpeg_len);
		goto fail;
	}
	EMIT("\nendstream\nendobj\n");

	startxref = ftell(out);
	EMIT("xref\n0 6\n");
	EMIT("0000000000 65535 f \n");
	for (i = 1; i <= 5; i++)
		EMIT("%010ld 00000 n \n", offsets[i]);
	EMIT("trailer\n<</Size 6/Root 1 0 R>>\nstartxref\n%ld\n%%%%EOF\n",
	     startxref);

#undef EMIT

	fclose(in);
	/* Check the close: buffered writes can fail here and nowhere else, and
	 * a truncated PDF that reports success is worse than a clear error. */
	if (fclose(out)) {
		fprintf(stderr, "jpeg2pdf: %s: %s\n", out_path, strerror(errno));
		remove(out_path);
		return 1;
	}
	return 0;

wr_err:
	fprintf(stderr, "jpeg2pdf: %s: %s\n", out_path, strerror(errno));
fail:
	fclose(in);
	fclose(out);
	remove(out_path);
	return 1;
}
