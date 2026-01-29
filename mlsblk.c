/*
 * mlsblk - list block devices (macOS port of lsblk)
 * Data: diskutil list -plist, getmntinfo(), diskutil info -plist (for -f)
 */

#define _DARWIN_C_SOURCE
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

/* Default columns when no -o */
#define DEFAULT_COLS "NAME,SIZE,TYPE,MOUNTPOINT"

typedef struct Node Node;
struct Node {
	char *name;           /* disk0, disk0s1, ... */
	uint64_t size;        /* bytes */
	char *type;           /* "disk" or "part" */
	char *mountpoint;     /* path or "" */
	char *fstype;         /* apfs, hfs, etc */
	char *label;          /* volume name */
	char *uuid;           /* UUID string */
	Node *parent;
	Node **children;
	int nchildren;
	int cap_children;
	int index;            /* for stable sort */
};

static Node *node_create(const char *name, uint64_t size, const char *type) {
	Node *n = calloc(1, sizeof(Node));
	if (!n) return NULL;
	n->name = strdup(name);
	n->size = size;
	n->type = strdup(type ? type : "disk");
	n->mountpoint = strdup("");
	n->fstype = strdup("");
	n->label = strdup("");
	n->uuid = strdup("");
	n->children = NULL;
	n->cap_children = 0;
	return n;
}

static void node_free(Node *n) {
	if (!n) return;
	free(n->name);
	free(n->type);
	free(n->mountpoint);
	free(n->fstype);
	free(n->label);
	free(n->uuid);
	for (int i = 0; i < n->nchildren; i++)
		node_free(n->children[i]);
	free(n->children);
	free(n);
}

static int node_add_child(Node *parent, Node *child) {
	if (parent->nchildren >= parent->cap_children) {
		int newcap = parent->cap_children ? parent->cap_children * 2 : 4;
		Node **p = realloc(parent->children, (size_t)newcap * sizeof(Node *));
		if (!p) return -1;
		parent->children = p;
		parent->cap_children = newcap;
	}
	child->parent = parent;
	parent->children[parent->nchildren++] = child;
	return 0;
}

/* Parse "disk0", "disk0s1" -> compare numerically */
static int name_cmp(const char *a, const char *b) {
	if (!a || !b) return 0;
	/* skip "disk" prefix */
	const char *pa = (strncmp(a, "disk", 4) == 0) ? a + 4 : a;
	const char *pb = (strncmp(b, "disk", 4) == 0) ? b + 4 : b;
	while (*pa && *pb) {
		if (*pa == *pb) { pa++; pb++; continue; }
		if (*pa == 's' && *pb == 's') { pa++; pb++; continue; }
		if (*pa == 's') return 1;
		if (*pb == 's') return -1;
		if (*pa >= '0' && *pa <= '9' && *pb >= '0' && *pb <= '9') {
			unsigned long na = strtoul(pa, (char **)&pa, 10);
			unsigned long nb = strtoul(pb, (char **)&pb, 10);
			if (na != nb) return (na > nb) - (na < nb);
			continue;
		}
		return (unsigned char)*pa - (unsigned char)*pb;
	}
	return (unsigned char)*pa - (unsigned char)*pb;
}

static int node_cmp(const void *va, const void *vb) {
	const Node *a = *(const Node *const *)va;
	const Node *b = *(const Node *const *)vb;
	return name_cmp(a->name, b->name);
}

static void sort_children(Node *n) {
	if (n->nchildren <= 1) return;
	qsort(n->children, (size_t)n->nchildren, sizeof(Node *), node_cmp);
	for (int i = 0; i < n->nchildren; i++)
		sort_children(n->children[i]);
}

/* Human-readable size */
static void fmt_size(uint64_t bytes, char *buf, size_t bufsz) {
	const char *units[] = { "B", "K", "M", "G", "T", "P" };
	int u = 0;
	double v = (double)bytes;
	while (v >= 1024 && u < 5) { v /= 1024; u++; }
	snprintf(buf, bufsz, "%.1f%c", v, *units[u]);
}

/* Run diskutil list -plist, return CFDictionary or NULL */
static CFDictionaryRef get_list_plist(void) {
	FILE *fp = popen("diskutil list -plist 2>/dev/null", "r");
	if (!fp) return NULL;
	size_t cap = 65536, len = 0;
	char *buf = malloc(cap);
	if (!buf) { pclose(fp); return NULL; }
	while (!feof(fp)) {
		len += fread(buf + len, 1, cap - len, fp);
		if (len >= cap - 1024) {
			cap *= 2;
			char *n = realloc(buf, cap);
			if (!n) { free(buf); pclose(fp); return NULL; }
			buf = n;
		}
	}
	pclose(fp);
	buf[len] = '\0';

	CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (const UInt8 *)buf, (CFIndex)len, kCFAllocatorNull);
	if (!data) { free(buf); return NULL; }
	CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	free(buf);
	if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		if (plist) CFRelease(plist);
		return NULL;
	}
	return (CFDictionaryRef)plist;
}

/* Run diskutil info -plist <device>, return CFDictionary (caller releases) */
static CFDictionaryRef get_info_plist(const char *device) {
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "diskutil info -plist %s 2>/dev/null", device);
	FILE *fp = popen(cmd, "r");
	if (!fp) return NULL;
	size_t cap = 32768, len = 0;
	char *buf = malloc(cap);
	if (!buf) { pclose(fp); return NULL; }
	while (!feof(fp)) {
		len += fread(buf + len, 1, cap - len, fp);
		if (len >= cap - 1024) {
			cap *= 2;
			char *n = realloc(buf, cap);
			if (!n) { free(buf); pclose(fp); return NULL; }
			buf = n;
		}
	}
	pclose(fp);
	buf[len] = '\0';

	CFDataRef data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (const UInt8 *)buf, (CFIndex)len, kCFAllocatorNull);
	if (!data) { free(buf); return NULL; }
	CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	free(buf);
	if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		if (plist) CFRelease(plist);
		return NULL;
	}
	return (CFDictionaryRef)plist;
}

static char *cfstr(CFTypeRef ref) {
	if (!ref || CFGetTypeID(ref) != CFStringGetTypeID()) return NULL;
	CFStringRef s = (CFStringRef)ref;
	CFIndex len = CFStringGetLength(s);
	CFIndex size = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
	char *buf = malloc((size_t)size);
	if (!buf) return NULL;
	if (!CFStringGetCString(s, buf, size, kCFStringEncodingUTF8)) { free(buf); return NULL; }
	return buf;
}

static uint64_t cfint(CFTypeRef ref) {
	if (!ref) return 0;
	if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
		long long val = 0;
		CFNumberGetValue((CFNumberRef)ref, kCFNumberLongLongType, &val);
		return (uint64_t)val;
	}
	return 0;
}

/* Content string -> fstype for display */
static void content_to_fstype(const char *content, char *out, size_t outsz) {
	if (!content) { out[0] = '\0'; return; }
	if (strstr(content, "APFS") || strstr(content, "41504653")) { snprintf(out, outsz, "apfs"); return; }
	if (strstr(content, "HFS") || strstr(content, "Apple_HFS")) { snprintf(out, outsz, "hfs"); return; }
	if (strstr(content, "EFI") || strstr(content, "C12A7328")) { snprintf(out, outsz, "vfat"); return; }
	if (strstr(content, "GUID_partition_scheme")) { out[0] = '\0'; return; }
	snprintf(out, outsz, "%.31s", content);
}

static void set_mountpoint_recursive(Node *node, const char *from, const char *target) {
	if (strcmp(node->name, from) == 0) {
		free(node->mountpoint);
		node->mountpoint = strdup(target);
		return;
	}
	for (int j = 0; j < node->nchildren; j++)
		set_mountpoint_recursive(node->children[j], from, target);
}

/* Build mount map from getmntinfo */
static void fill_mountpoints(Node **roots, int nroots) {
	struct statfs *mntbuf = NULL;
	int count = getmntinfo(&mntbuf, MNT_NOWAIT);
	if (count <= 0 || !mntbuf) return;

	for (int i = 0; i < count; i++) {
		const char *from = mntbuf[i].f_mntfromname;
		const char *target = mntbuf[i].f_mntonname;
		if (!from || !target) continue;
		/* from is like /dev/disk1s1 */
		if (strncmp(from, "/dev/", 5) != 0) continue;
		from += 5;

		for (int r = 0; r < nroots; r++)
			set_mountpoint_recursive(roots[r], from, target);
	}
}

/* Fill node from diskutil info -plist (for -f) */
static void fill_info(Node *n) {
	CFDictionaryRef info = get_info_plist(n->name);
	if (!info) return;
	CFTypeRef v;
	v = CFDictionaryGetValue(info, CFSTR("FilesystemType"));
	if (v) {
		char *s = cfstr(v);
		if (s) { free(n->fstype); n->fstype = s; }
	}
	v = CFDictionaryGetValue(info, CFSTR("VolumeName"));
	if (v) {
		char *s = cfstr(v);
		if (s && s[0]) { free(n->label); n->label = s; } else free(s);
	}
	if (!n->label || !n->label[0]) {
		v = CFDictionaryGetValue(info, CFSTR("MediaName"));
		if (v) { char *s = cfstr(v); if (s && s[0]) { free(n->label); n->label = s; } else free(s); }
	}
	v = CFDictionaryGetValue(info, CFSTR("VolumeUUID"));
	if (!v) v = CFDictionaryGetValue(info, CFSTR("DiskUUID"));
	if (v) {
		char *s = cfstr(v);
		if (s) { free(n->uuid); n->uuid = s; }
	}
	v = CFDictionaryGetValue(info, CFSTR("MountPoint"));
	if (v) {
		char *s = cfstr(v);
		if (s && s[0]) { free(n->mountpoint); n->mountpoint = s; } else free(s);
	}
	CFRelease(info);
}

/* Recursively collect all nodes from AllDisksAndPartitions into a flat list and tree */
typedef struct { Node **arr; int n, cap; } NodeArray;
static void collect_nodes(CFArrayRef all, Node **roots, int *nroots, NodeArray *flat, Node *parent);

static Node *ensure_node(NodeArray *flat, const char *name, uint64_t size, const char *type) {
	for (int i = 0; i < flat->n; i++)
		if (strcmp(flat->arr[i]->name, name) == 0)
			return flat->arr[i];
	Node *n = node_create(name, size, type);
	if (!n) return NULL;
	if (flat->n >= flat->cap) {
		int newcap = flat->cap ? flat->cap * 2 : 64;
		Node **p = realloc(flat->arr, (size_t)newcap * sizeof(Node *));
		if (!p) { node_free(n); return NULL; }
		flat->arr = p;
		flat->cap = newcap;
	}
	flat->arr[flat->n++] = n;
	return n;
}

static void add_partition(NodeArray *flat, Node **roots, int *nroots, Node *disk_node, CFDictionaryRef part) {
	(void)roots;
	(void)nroots;
	CFTypeRef idref = CFDictionaryGetValue(part, CFSTR("DeviceIdentifier"));
	CFTypeRef sizeref = CFDictionaryGetValue(part, CFSTR("Size"));
	CFTypeRef content = CFDictionaryGetValue(part, CFSTR("Content"));
	char *idstr = cfstr(idref);
	if (!idstr) return;
	uint64_t sz = cfint(sizeref);
	char *contentstr = content ? cfstr(content) : NULL;
	Node *child = ensure_node(flat, idstr, sz, "part");
	free(idstr);
	if (child) {
		char fstype[64];
		content_to_fstype(contentstr, fstype, sizeof(fstype));
		free(child->fstype);
		child->fstype = strdup(fstype);
		if (disk_node)
			node_add_child(disk_node, child);
	}
	free(contentstr);
}

static void add_apfs_volume(NodeArray *flat, Node *container_node, CFDictionaryRef vol) {
	CFTypeRef idref = CFDictionaryGetValue(vol, CFSTR("DeviceIdentifier"));
	CFTypeRef sizeref = CFDictionaryGetValue(vol, CFSTR("Size"));
	CFTypeRef mount = CFDictionaryGetValue(vol, CFSTR("MountPoint"));
	CFTypeRef volname = CFDictionaryGetValue(vol, CFSTR("VolumeName"));
	CFTypeRef voluuid = CFDictionaryGetValue(vol, CFSTR("VolumeUUID"));
	char *idstr = cfstr(idref);
	if (!idstr) return;
	uint64_t sz = cfint(sizeref);
	Node *child = ensure_node(flat, idstr, sz, "part");
	free(idstr);
	if (!child) return;
	if (container_node)
		node_add_child(container_node, child);
	char *mp = mount ? cfstr(mount) : NULL;
	if (mp && mp[0]) { free(child->mountpoint); child->mountpoint = mp; } else free(mp);
	char *lab = volname ? cfstr(volname) : NULL;
	if (lab && lab[0]) { free(child->label); child->label = lab; } else free(lab);
	char *uuid = voluuid ? cfstr(voluuid) : NULL;
	if (uuid) { free(child->uuid); child->uuid = uuid; }
	child->fstype = realloc(child->fstype, 5);
	if (child->fstype) strcpy(child->fstype, "apfs");
}

static void collect_nodes(CFArrayRef all, Node **roots, int *nroots, NodeArray *flat, Node *parent) {
	CFIndex cnt = CFArrayGetCount(all);
	for (CFIndex i = 0; i < cnt; i++) {
		CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(all, i);
		if (CFGetTypeID(d) != CFDictionaryGetTypeID()) continue;

		CFTypeRef idref = CFDictionaryGetValue(d, CFSTR("DeviceIdentifier"));
		CFTypeRef sizeref = CFDictionaryGetValue(d, CFSTR("Size"));
		CFTypeRef content = CFDictionaryGetValue(d, CFSTR("Content"));
		char *idstr = cfstr(idref);
		if (!idstr) continue;
		uint64_t sz = cfint(sizeref);
		char *contentstr = content ? cfstr(content) : NULL;
		/* Whole disk or APFS container */
		bool is_container = contentstr && strstr(contentstr, "Apple_APFS_Container");
		bool is_whole = contentstr && (strstr(contentstr, "GUID_partition_scheme") || is_container);

		Node *disk_node = ensure_node(flat, idstr, sz, is_whole ? "disk" : "part");
		free(idstr);
		if (!disk_node) { free(contentstr); continue; }
		if (contentstr) {
			char buf[64];
			content_to_fstype(contentstr, buf, sizeof(buf));
			free(disk_node->fstype);
			disk_node->fstype = strdup(buf);
		}

		if (!parent) {
			if (*nroots >= 64) { free(contentstr); continue; }
			roots[(*nroots)++] = disk_node;
		} else
			node_add_child(parent, disk_node);

		/* Partitions (physical) */
		CFArrayRef parts = (CFArrayRef)CFDictionaryGetValue(d, CFSTR("Partitions"));
		if (parts && CFGetTypeID(parts) == CFArrayGetTypeID()) {
			CFIndex np = CFArrayGetCount(parts);
			for (CFIndex j = 0; j < np; j++)
				add_partition(flat, roots, nroots, disk_node, (CFDictionaryRef)CFArrayGetValueAtIndex(parts, j));
		}

		/* APFS volumes */
		CFArrayRef apfs_vols = (CFArrayRef)CFDictionaryGetValue(d, CFSTR("APFSVolumes"));
		if (apfs_vols && CFGetTypeID(apfs_vols) == CFArrayGetTypeID()) {
			CFIndex nv = CFArrayGetCount(apfs_vols);
			for (CFIndex j = 0; j < nv; j++)
				add_apfs_volume(flat, disk_node, (CFDictionaryRef)CFArrayGetValueAtIndex(apfs_vols, j));
		}

		free(contentstr);
	}
}

/* Parse AllDisksAndPartitions and build tree. Roots are top-level disks. */
static int build_tree(CFDictionaryRef list_plist, Node **roots, int *nroots, NodeArray *flat) {
	CFArrayRef all = (CFArrayRef)CFDictionaryGetValue(list_plist, CFSTR("AllDisksAndPartitions"));
	if (!all || CFGetTypeID(all) != CFArrayGetTypeID()) return -1;
	flat->arr = NULL;
	flat->n = flat->cap = 0;
	*nroots = 0;
	collect_nodes(all, roots, nroots, flat, NULL);
	/* Sort roots and each level of children */
	for (int i = 0; i < *nroots; i++)
		sort_children(roots[i]);
	qsort(roots, (size_t)*nroots, sizeof(Node *), node_cmp);
	return 0;
}

/* Column names we support */
enum Col { COL_NAME, COL_SIZE, COL_TYPE, COL_MOUNTPOINT, COL_FSTYPE, COL_LABEL, COL_UUID, COL_MAX };
static const char *col_names[] = { "NAME", "SIZE", "TYPE", "MOUNTPOINT", "FSTYPE", "LABEL", "UUID" };

static int parse_columns(const char *ostr, int *cols, int *ncols) {
	*ncols = 0;
	char *s = strdup(ostr);
	if (!s) return -1;
	for (char *tok = strtok(s, ","); tok; tok = strtok(NULL, ",")) {
		while (*tok == ' ') tok++;
		for (int c = 0; c < COL_MAX; c++)
			if (strcasecmp(tok, col_names[c]) == 0) {
				if (*ncols >= 32) { free(s); return -1; }
				cols[(*ncols)++] = c;
				break;
			}
	}
	free(s);
	return 0;
}

static int parse_output_option(const char *ostr, int *cols, int *ncols) {
	if (!ostr || !ostr[0]) {
		const char *def = DEFAULT_COLS;
		return parse_columns(def, cols, ncols);
	}
	return parse_columns(ostr, cols, ncols);
}

static void print_tree(Node *n, int *cols, int ncols, const char *prefix, bool last) {
	char sizebuf[32];
	char child_prefix[256];
	snprintf(child_prefix, sizeof(child_prefix), "%s%s  ", prefix, last ? " " : "│");

	for (int i = 0; i < n->nchildren; i++) {
		Node *ch = n->children[i];
		bool is_last = (i == n->nchildren - 1);
		fmt_size(ch->size, sizebuf, sizeof(sizebuf));
		printf("%s%s── %s", prefix, is_last ? "└" : "├", ch->name);
		for (int c = 1; c < ncols; c++) {
			switch (cols[c]) {
			case COL_NAME: break;
			case COL_SIZE: printf(" %s", sizebuf); break;
			case COL_TYPE: printf(" %s", ch->type); break;
			case COL_MOUNTPOINT: printf(" %s", ch->mountpoint[0] ? ch->mountpoint : ""); break;
			case COL_FSTYPE: printf(" %s", ch->fstype[0] ? ch->fstype : ""); break;
			case COL_LABEL: printf(" %s", ch->label[0] ? ch->label : ""); break;
			case COL_UUID: printf(" %s", ch->uuid[0] ? ch->uuid : ""); break;
			default: break;
			}
		}
		printf("\n");
		print_tree(ch, cols, ncols, child_prefix, is_last);
	}
}

static void print_list_dfs(Node *n, int *cols, int ncols, char *sizebuf) {
	fmt_size(n->size, sizebuf, sizeof(sizebuf));
	for (int c = 0; c < ncols; c++) {
		if (c) printf(" ");
		switch (cols[c]) {
		case COL_NAME: printf("%s", n->name); break;
		case COL_SIZE: printf("%s", sizebuf); break;
		case COL_TYPE: printf("%s", n->type); break;
		case COL_MOUNTPOINT: printf("%s", n->mountpoint[0] ? n->mountpoint : ""); break;
		case COL_FSTYPE: printf("%s", n->fstype[0] ? n->fstype : ""); break;
		case COL_LABEL: printf("%s", n->label[0] ? n->label : ""); break;
		case COL_UUID: printf("%s", n->uuid[0] ? n->uuid : ""); break;
		default: break;
		}
	}
	printf("\n");
	for (int j = 0; j < n->nchildren; j++)
		print_list_dfs(n->children[j], cols, ncols, sizebuf);
}

static void print_list(Node **roots, int nroots, int *cols, int ncols) {
	for (int c = 0; c < ncols; c++)
		printf("%s%s", c ? " " : "", col_names[cols[c]]);
	printf("\n");
	char sizebuf[32];
	for (int i = 0; i < nroots; i++)
		print_list_dfs(roots[i], cols, ncols, sizebuf);
}

static void emit_json(Node *n, int depth, bool first) {
	if (!first) printf(",\n");
	printf("%*s{\"name\":\"%s\",\"size\":%llu,\"type\":\"%s\",\"mountpoint\":\"%s\",\"fstype\":\"%s\",\"label\":\"%s\",\"uuid\":\"%s\"",
		depth * 2, "", n->name, (unsigned long long)n->size, n->type,
		n->mountpoint[0] ? n->mountpoint : "", n->fstype[0] ? n->fstype : "",
		n->label[0] ? n->label : "", n->uuid[0] ? n->uuid : "");
	if (n->nchildren > 0) {
		printf(",\"children\":[");
		for (int i = 0; i < n->nchildren; i++)
			emit_json(n->children[i], depth + 1, i == 0);
		printf("\n%*s]", depth * 2, "");
	}
	printf("}");
}

static void print_json(Node **roots, int nroots) {
	printf("{\"blockdevices\":[\n");
	for (int i = 0; i < nroots; i++) {
		if (i) printf(",\n");
		emit_json(roots[i], 1, true);
	}
	printf("\n]}\n");
}

int main(int argc, char **argv) {
	bool opt_f = false;
	bool opt_J = false;
	bool opt_list = false;
	char *opt_o = NULL;

	int ch;
	while ((ch = getopt(argc, argv, "fo:Jl")) != -1) {
		switch (ch) {
		case 'f': opt_f = true; break;
		case 'o': opt_o = optarg; break;
		case 'J': opt_J = true; break;
		case 'l': opt_list = true; break;
		default:
			fprintf(stderr, "Usage: mlsblk [-f] [-o COL1,COL2] [-J] [-l]\n");
			fprintf(stderr, "  -f  include FSTYPE,LABEL,UUID\n");
			fprintf(stderr, "  -o  output columns (e.g. NAME,SIZE,FSTYPE,MOUNTPOINT)\n");
			fprintf(stderr, "  -J  JSON output\n");
			fprintf(stderr, "  -l  list format instead of tree\n");
			return 1;
		}
	}

	int cols[32], ncols = 0;
	if (parse_output_option(opt_o, cols, &ncols) != 0) {
		fprintf(stderr, "mlsblk: invalid -o columns\n");
		return 1;
	}
	if (opt_f && !opt_o) {
		parse_columns("NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT,LABEL,UUID", cols, &ncols);
	}

	CFDictionaryRef list_plist = get_list_plist();
	if (!list_plist) {
		fprintf(stderr, "mlsblk: failed to run diskutil list -plist\n");
		return 1;
	}

	Node *roots[64];
	int nroots = 0;
	NodeArray flat = { 0 };
	if (build_tree(list_plist, roots, &nroots, &flat) != 0) {
		fprintf(stderr, "mlsblk: failed to parse disk list\n");
		CFRelease(list_plist);
		return 1;
	}
	CFRelease(list_plist);  /* done with plist */

	fill_mountpoints(roots, nroots);

	if (opt_f) {
		for (int i = 0; i < flat.n; i++)
			fill_info(flat.arr[i]);
	}

	if (opt_J) {
		print_json(roots, nroots);
	} else if (opt_list) {
		print_list(roots, nroots, cols, ncols);
	} else {
		/* Tree: print header then each root */
		for (int c = 0; c < ncols; c++)
			printf("%s%s", c ? " " : "", col_names[cols[c]]);
		printf("\n");
		char sizebuf[32];
		for (int i = 0; i < nroots; i++) {
			fmt_size(roots[i]->size, sizebuf, sizeof(sizebuf));
			printf("%s", roots[i]->name);
			for (int c = 1; c < ncols; c++) {
				switch (cols[c]) {
				case COL_NAME: break;
				case COL_SIZE: printf(" %s", sizebuf); break;
				case COL_TYPE: printf(" %s", roots[i]->type); break;
				case COL_MOUNTPOINT: printf(" %s", roots[i]->mountpoint[0] ? roots[i]->mountpoint : ""); break;
				case COL_FSTYPE: printf(" %s", roots[i]->fstype[0] ? roots[i]->fstype : ""); break;
				case COL_LABEL: printf(" %s", roots[i]->label[0] ? roots[i]->label : ""); break;
				case COL_UUID: printf(" %s", roots[i]->uuid[0] ? roots[i]->uuid : ""); break;
				default: break;
				}
			}
			printf("\n");
			print_tree(roots[i], cols, ncols, "  ", i == nroots - 1 && roots[i]->nchildren == 0);
		}
	}

	/* Free tree via roots only (nodes are shared with flat.arr) */
	for (int i = 0; i < nroots; i++)
		node_free(roots[i]);
	free(flat.arr);
	return 0;
}
