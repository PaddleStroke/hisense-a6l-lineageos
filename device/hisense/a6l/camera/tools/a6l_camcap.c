// SPDX-License-Identifier: GPL-2.0
/*
 * a6l_camcap: minimal media-ctl + yavta replacement for the A6L camera bring-up
 * (static NDK binary, no libs). Qualcomm CAMSS (sdm660) RDI raw capture.
 *
 *   a6l_camcap -l                          list media entities / pads / links
 *   a6l_camcap -s <sensor-substr> -p <phy> -c <csid> -W <w> -H <h> [-e exp] [-g gain]
 *              [-t testpattern] [-n skip] [-o out.raw]
 *   a6l_camcap -s gt9769 -f 0,256,512,768,1023   sweep VCM focus (1 s per step, subdev kept open)
 *
 * Pipeline: sensor -> msm_csiphy<p> -> msm_csid<c> -> msm_ispif<c> -> msm_vfe0_rdi0 -> msm_vfe0_video0
 * Output file = one frame of MIPI-packed RAW10 (bytesperline from the driver, printed).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

#define MAXE 64
static struct media_entity_desc ents[MAXE];
static int nents;
static int mfd;

static void die(const char *m) { perror(m); exit(1); }

static void load_entities(void)
{
	struct media_entity_desc e;
	memset(&e, 0, sizeof(e));
	e.id = MEDIA_ENT_ID_FLAG_NEXT;
	while (nents < MAXE && ioctl(mfd, MEDIA_IOC_ENUM_ENTITIES, &e) == 0) {
		ents[nents++] = e;
		e.id |= MEDIA_ENT_ID_FLAG_NEXT;
	}
}

static struct media_entity_desc *find(const char *name, int exact)
{
	for (int i = 0; i < nents; i++)
		if (exact ? !strcmp(ents[i].name, name) : !!strstr(ents[i].name, name))
			return &ents[i];
	return NULL;
}

static char *devnode(struct media_entity_desc *e, char *buf, size_t n)
{
	const char *dirs = "/dev";
	DIR *d = opendir(dirs);
	struct dirent *de;
	struct stat st;
	char p[300];

	if (!d)
		return NULL;
	while ((de = readdir(d))) {
		snprintf(p, sizeof(p), "%s/%s", dirs, de->d_name);
		if (stat(p, &st) == 0 && S_ISCHR(st.st_mode) &&
		    major(st.st_rdev) == e->dev.major && minor(st.st_rdev) == e->dev.minor &&
		    (strstr(de->d_name, "v4l-subdev") || strstr(de->d_name, "video"))) {
			snprintf(buf, n, "%s", p);
			closedir(d);
			return buf;
		}
	}
	closedir(d);
	return NULL;
}

static void list(void)
{
	for (int i = 0; i < nents; i++) {
		struct media_entity_desc *e = &ents[i];
		struct media_pad_desc pads[16];
		struct media_link_desc links[32];
		struct media_links_enum le = { .entity = e->id, .pads = pads, .links = links };
		char dn[300] = "-";

		devnode(e, dn, sizeof(dn));
		printf("entity %u '%s' type 0x%x pads %u links %u dev %u:%u %s\n", e->id, e->name,
		       e->type, e->pads, e->links, e->dev.major, e->dev.minor, dn);
		if (e->links > 32 || e->pads > 16)
			continue;
		if (ioctl(mfd, MEDIA_IOC_ENUM_LINKS, &le) == 0)
			for (unsigned j = 0; j < e->links; j++) {
				struct media_entity_desc *s = NULL, *t = NULL;
				for (int k = 0; k < nents; k++) {
					if (ents[k].id == links[j].source.entity) s = &ents[k];
					if (ents[k].id == links[j].sink.entity) t = &ents[k];
				}
				printf("   link '%s':%u -> '%s':%u flags 0x%x\n", s ? s->name : "?",
				       links[j].source.index, t ? t->name : "?", links[j].sink.index,
				       links[j].flags);
			}
	}
}

static void mlink(struct media_entity_desc *a, int pa, struct media_entity_desc *b, int pb)
{
	struct media_link_desc l;
	memset(&l, 0, sizeof(l));
	l.source.entity = a->id; l.source.index = pa; l.source.flags = MEDIA_PAD_FL_SOURCE;
	l.sink.entity = b->id; l.sink.index = pb; l.sink.flags = MEDIA_PAD_FL_SINK;
	l.flags = MEDIA_LNK_FL_ENABLED;
	if (ioctl(mfd, MEDIA_IOC_SETUP_LINK, &l) < 0)
		fprintf(stderr, "link %s:%d -> %s:%d: %s\n", a->name, pa, b->name, pb, strerror(errno));
	else
		printf("linked %s:%d -> %s:%d\n", a->name, pa, b->name, pb);
}

static void reset_links(void)
{
	for (int i = 0; i < nents; i++) {
		struct media_pad_desc pads[16];
		struct media_link_desc links[32];
		struct media_links_enum le = { .entity = ents[i].id, .pads = pads, .links = links };
		if (ents[i].links > 32 || ents[i].pads > 16) continue;
		if (ioctl(mfd, MEDIA_IOC_ENUM_LINKS, &le)) continue;
		for (unsigned j = 0; j < ents[i].links; j++) {
			struct media_link_desc l = links[j];
			if (!(l.flags & MEDIA_LNK_FL_ENABLED) || (l.flags & MEDIA_LNK_FL_IMMUTABLE)) continue;
			if (l.source.entity != ents[i].id) continue;
			l.flags = 0;
			if (ioctl(mfd, MEDIA_IOC_SETUP_LINK, &l) == 0) printf("unlinked %u:%u -> %u:%u\n", l.source.entity, l.source.index, l.sink.entity, l.sink.index);
		}
	}
}

static int subdev_fd(struct media_entity_desc *e)
{
	char dn[300];
	int fd;
	if (!devnode(e, dn, sizeof(dn))) { fprintf(stderr, "no devnode for %s\n", e->name); exit(1); }
	fd = open(dn, O_RDWR);
	if (fd < 0) die(dn);
	return fd;
}

static uint32_t set_fmt(struct media_entity_desc *e, int pad, uint32_t code, int w, int h)
{
	struct v4l2_subdev_format f;
	int fd = subdev_fd(e);
	memset(&f, 0, sizeof(f));
	f.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	f.pad = pad;
	if (ioctl(fd, VIDIOC_SUBDEV_G_FMT, &f) < 0) die("G_FMT");
	if (code) f.format.code = code;
	f.format.width = w; f.format.height = h;
	f.format.field = V4L2_FIELD_NONE;
	if (ioctl(fd, VIDIOC_SUBDEV_S_FMT, &f) < 0) die("S_FMT");
	printf("fmt %s:%d = %ux%u code 0x%x\n", e->name, pad, f.format.width, f.format.height, f.format.code);
	close(fd);
	return f.format.code;
}

static void set_ctrl(struct media_entity_desc *e, uint32_t id, int v, const char *n)
{
	struct v4l2_control c = { .id = id, .value = v };
	int fd = subdev_fd(e);
	if (ioctl(fd, VIDIOC_S_CTRL, &c) < 0) fprintf(stderr, "set %s: %s\n", n, strerror(errno));
	else printf("ctrl %s = %d\n", n, v);
	close(fd);
}

static uint32_t pixfmt(uint32_t code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SRGGB10_1X10: return V4L2_PIX_FMT_SRGGB10P;
	case MEDIA_BUS_FMT_SGRBG10_1X10: return V4L2_PIX_FMT_SGRBG10P;
	case MEDIA_BUS_FMT_SGBRG10_1X10: return V4L2_PIX_FMT_SGBRG10P;
	case MEDIA_BUS_FMT_SBGGR10_1X10: return V4L2_PIX_FMT_SBGGR10P;
	}
	fprintf(stderr, "unknown mbus code 0x%x\n", code);
	exit(1);
}

int main(int argc, char **argv)
{
	const char *media = "/dev/media0", *sname = NULL, *out = "/tmp/frame.raw";
	int phy = 0, csid = 0, W = 0, H = 0, exp = -1, gain = -1, tp = -1, skip = 4, lst = 0, o;
	char nm[64];
	char *focus = NULL;

	while ((o = getopt(argc, argv, "m:ls:p:c:W:H:e:g:t:n:o:f:")) != -1) {
		switch (o) {
		case 'm': media = optarg; break;
		case 'l': lst = 1; break;
		case 's': sname = optarg; break;
		case 'p': phy = atoi(optarg); break;
		case 'c': csid = atoi(optarg); break;
		case 'W': W = atoi(optarg); break;
		case 'H': H = atoi(optarg); break;
		case 'e': exp = atoi(optarg); break;
		case 'g': gain = atoi(optarg); break;
		case 't': tp = atoi(optarg); break;
		case 'n': skip = atoi(optarg); break;
		case 'o': out = optarg; break;
		case 'f': focus = optarg; break;
		default: fprintf(stderr, "see source header for usage\n"); return 2;
		}
	}
	mfd = open(media, O_RDWR);
	if (mfd < 0) die(media);
	load_entities();
	if (lst || !sname) { list(); return 0; }

	struct media_entity_desc *sen = find(sname, 0), *ph, *cs, *isp, *rdi, *vid;
	if (focus) {
		if (!sen) { fprintf(stderr, "no entity '%s'\n", sname); return 1; }
		int fd = subdev_fd(sen);
		for (char *t = strtok(focus, ","); t; t = strtok(NULL, ",")) {
			struct v4l2_control c = { .id = V4L2_CID_FOCUS_ABSOLUTE, .value = atoi(t) };
			if (ioctl(fd, VIDIOC_S_CTRL, &c) < 0) perror("FOCUS_ABSOLUTE");
			else printf("A6L_CAMCAP_FOCUS %d\n", c.value);
			sleep(1);
		}
		close(fd);
		return 0;
	}
	snprintf(nm, sizeof(nm), "msm_csiphy%d", phy); ph = find(nm, 1);
	snprintf(nm, sizeof(nm), "msm_csid%d", csid); cs = find(nm, 1);
	snprintf(nm, sizeof(nm), "msm_ispif%d", csid); isp = find(nm, 1);
	rdi = find("msm_vfe0_rdi0", 1); vid = find("msm_vfe0_video0", 1);
	if (!sen || !ph || !cs || !rdi || !vid) { fprintf(stderr, "missing entity (use -l)\n"); list(); return 1; }

	reset_links();
	mlink(ph, 1, cs, 0);
	if (isp) { mlink(cs, 1, isp, 0); mlink(isp, 1, rdi, 0); } else mlink(cs, 1, rdi, 0); /* sdm660: no ISPIF */

	if (tp >= 0) set_ctrl(sen, V4L2_CID_TEST_PATTERN, tp, "test_pattern");
	uint32_t code = set_fmt(sen, 0, 0, W, H);
	set_fmt(ph, 0, code, W, H); set_fmt(cs, 0, code, W, H);
	if (isp) set_fmt(isp, 0, code, W, H);
	set_fmt(rdi, 0, code, W, H);
	if (exp >= 0) set_ctrl(sen, V4L2_CID_EXPOSURE, exp, "exposure");
	if (gain >= 0) set_ctrl(sen, V4L2_CID_ANALOGUE_GAIN, gain, "analogue_gain");

	char vn[300];
	if (!devnode(vid, vn, sizeof(vn))) { fprintf(stderr, "no video node\n"); return 1; }
	int vfd = open(vn, O_RDWR);
	if (vfd < 0) die(vn);

	struct v4l2_format f;
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f.fmt.pix_mp.width = W; f.fmt.pix_mp.height = H;
	f.fmt.pix_mp.pixelformat = pixfmt(code);
	f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	f.fmt.pix_mp.num_planes = 1;
	if (ioctl(vfd, VIDIOC_S_FMT, &f) < 0) die("VIDIOC_S_FMT");
	printf("video %s: %ux%u fourcc %.4s bpl %u size %u\n", vn, f.fmt.pix_mp.width, f.fmt.pix_mp.height,
	       (char *)&f.fmt.pix_mp.pixelformat, f.fmt.pix_mp.plane_fmt[0].bytesperline,
	       f.fmt.pix_mp.plane_fmt[0].sizeimage);

	struct v4l2_requestbuffers rb = { .count = 4, .type = f.type, .memory = V4L2_MEMORY_MMAP };
	if (ioctl(vfd, VIDIOC_REQBUFS, &rb) < 0) die("REQBUFS");
	void *mem[8]; size_t len[8];
	for (unsigned i = 0; i < rb.count && i < 8; i++) {
		struct v4l2_plane pl[1]; struct v4l2_buffer b;
		memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
		b.type = f.type; b.memory = V4L2_MEMORY_MMAP; b.index = i; b.m.planes = pl; b.length = 1;
		if (ioctl(vfd, VIDIOC_QUERYBUF, &b) < 0) die("QUERYBUF");
		len[i] = pl[0].length;
		mem[i] = mmap(NULL, len[i], PROT_READ | PROT_WRITE, MAP_SHARED, vfd, pl[0].m.mem_offset);
		if (mem[i] == MAP_FAILED) die("mmap");
		if (ioctl(vfd, VIDIOC_QBUF, &b) < 0) die("QBUF");
	}
	int t = f.type;
	if (ioctl(vfd, VIDIOC_STREAMON, &t) < 0) die("STREAMON");
	printf("streaming...\n");
	for (int n = 0; n <= skip; n++) {
		struct v4l2_plane pl[1]; struct v4l2_buffer b;
		fd_set fs; struct timeval tv = { .tv_sec = 5 };
		FD_ZERO(&fs); FD_SET(vfd, &fs);
		if (select(vfd + 1, &fs, NULL, NULL, &tv) <= 0) { fprintf(stderr, "TIMEOUT waiting for frame %d\n", n); break; }
		memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
		b.type = f.type; b.memory = V4L2_MEMORY_MMAP; b.m.planes = pl; b.length = 1;
		if (ioctl(vfd, VIDIOC_DQBUF, &b) < 0) die("DQBUF");
		printf("frame %d seq %u bytes %u ts %ld.%06ld\n", n, b.sequence, pl[0].bytesused,
		       (long)b.timestamp.tv_sec, (long)b.timestamp.tv_usec);
		if (n == skip) {
			FILE *fo = fopen(out, "wb");
			if (!fo) die(out);
			fwrite(mem[b.index], 1, pl[0].bytesused ? pl[0].bytesused : len[b.index], fo);
			fclose(fo);
			printf("A6L_CAMCAP_FRAME_SAVED %s %ux%u bpl %u\n", out, W, H, f.fmt.pix_mp.plane_fmt[0].bytesperline);
		}
		if (ioctl(vfd, VIDIOC_QBUF, &b) < 0) die("QBUF2");
	}
	ioctl(vfd, VIDIOC_STREAMOFF, &t);
	return 0;
}
