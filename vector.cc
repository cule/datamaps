#include <iostream>
#include <fstream>
#include <string>
#include <zlib.h>
#include "vector_tile.pb.h"

#define XMAX 4096
#define YMAX 4096

extern "C" {
	#include "graphics.h"
	#include "clip.h"
}

struct line {
	int x0;
	int y0;
	int x1;
	int y1;
};

struct point {
	int x;
	int y;
};

#define MAX_POINTS 10000
struct pointlayer {
	struct point *points;
	int npoints;
	int npalloc;

	struct pointlayer *next;
};

struct metapointlayer {
	struct pointlayer *pointlayers;
	struct pointlayer **used;

	long long meta;
	struct metapointlayer *next;
};

struct linelayer {
	struct line *lines;
	int nlines;
	int nlalloc;
	unsigned char *used;

	struct linelayer *next;
};

struct metalinelayer {
	struct linelayer *linelayers;

	long long meta;
	struct metalinelayer *next;
};

class env {
public:
	mapnik::vector::tile tile;
	mapnik::vector::tile_layer *layer;
	mapnik::vector::tile_feature *feature;

	int x;
	int y;

	int cmd_idx;
	int cmd;
	int length;

	struct metapointlayer *metapointlayers;
	struct metalinelayer *metalinelayers;
};

#define MOVE_TO 1
#define LINE_TO 2
#define CLOSE_PATH 7
#define CMD_BITS 3

struct graphics {
	int width;
	int height;
	env *e;
};

struct pointlayer *new_pointlayer() {
	struct pointlayer *p = (struct pointlayer *) malloc(sizeof(struct pointlayer));
	p->npalloc = 1024;
	p->npoints = 0;
	p->points = (struct point *) malloc(p->npalloc * sizeof(struct point));
	p->next = NULL;

	return p;
}

struct metapointlayer *new_metapointlayer(long long meta, struct metapointlayer *next) {
	struct metapointlayer *mpl = (struct metapointlayer *) malloc(sizeof(struct metapointlayer));
	mpl->pointlayers = new_pointlayer();
	mpl->meta = meta;
	mpl->next = next;

	mpl->used = (struct pointlayer **) malloc(256 * 256 * sizeof(struct pointlayer *));
	int i;
	for (i = 0; i < 256 * 256; i++) {
		mpl->used[i] = mpl->pointlayers;
	}

	return mpl;
}

struct linelayer *new_linelayer() {
	struct linelayer *l = (struct linelayer *) malloc(sizeof(struct linelayer));
	l->nlalloc = 1024;
	l->nlines = 0;
	l->lines = (struct line *) malloc(l->nlalloc * sizeof(struct line));
	l->next = NULL;
	l->used = (unsigned char *) malloc(256 * 256 * sizeof(unsigned char));

	return l;
}

struct metalinelayer *new_metalinelayer(long long meta, struct metalinelayer *next) {
	struct metalinelayer *mll = (struct metalinelayer*) malloc(sizeof(struct metalinelayer));
	mll->linelayers = NULL;
	mll->meta = meta;
	mll->next = next;

	return mll;
}

struct graphics *graphics_init(int width, int height, char **filetype) {
	GOOGLE_PROTOBUF_VERIFY_VERSION;

	env *e = new env;

	e->metalinelayers = NULL;
	e->metapointlayers = NULL;

	struct graphics *g = (struct graphics *) malloc(sizeof(struct graphics));
	g->e = e;
	g->width = width;
	g->height = height;

	*filetype = strdup("pbf");
	return g;
}

// from mapnik-vector-tile/src/vector_tile_compression.hpp
static inline int compress(std::string const& input, std::string & output)
{
	z_stream deflate_s;
	deflate_s.zalloc = Z_NULL;
	deflate_s.zfree = Z_NULL;
	deflate_s.opaque = Z_NULL;
	deflate_s.avail_in = 0;
	deflate_s.next_in = Z_NULL;
	deflateInit(&deflate_s, Z_DEFAULT_COMPRESSION);
	deflate_s.next_in = (Bytef *)input.data();
	deflate_s.avail_in = input.size();
	size_t length = 0;
	do {
		size_t increase = input.size() / 2 + 1024;
		output.resize(length + increase);
		deflate_s.avail_out = increase;
		deflate_s.next_out = (Bytef *)(output.data() + length);
		int ret = deflate(&deflate_s, Z_FINISH);
		if (ret != Z_STREAM_END && ret != Z_OK && ret != Z_BUF_ERROR) {
			return -1;
		}
		length += (increase - deflate_s.avail_out);
	} while (deflate_s.avail_out == 0);
	deflateEnd(&deflate_s);
	output.resize(length);
	return 0;
}

static void op(env *e, int cmd, int x, int y);

void out(struct graphics *gc, int transparency, double gamma, int invert, int color, int color2, int saturate, int mask, double color_cap, int cie) {
	env *e = gc->e;
	int i;

	e->layer = e->tile.add_layers();
	e->layer->set_name("lines");
	e->layer->set_version(1);
	e->layer->set_extent(XMAX);

	e->layer->add_keys("meta", strlen("meta"));
	int vals = -1;

	struct metalinelayer *mll;
	for (mll = e->metalinelayers; mll != NULL; mll = mll->next) {
		mapnik::vector::tile_value *tv = e->layer->add_values();
		tv->set_int_value(mll->meta);
		vals++;

		struct linelayer *l;
		for (l = mll->linelayers; l != NULL; l = l->next){
			e->feature = e->layer->add_features();
			e->feature->set_type(mapnik::vector::tile::LineString);

			e->feature->add_tags(0); /* key, "meta" */
			e->feature->add_tags(vals); /* value */

			e->x = 0;
			e->y = 0;

			e->cmd_idx = -1;
			e->cmd = -1;
			e->length = 0;

			for (i = 0; i < l->nlines; i++) {
				// printf("draw %d %d to %d %d\n", e->lines[i].x0, e->lines[i].y0, e->lines[i].x1, e->lines[i].y1);

				if (l->lines[i].x0 != e->x || l->lines[i].y0 != e->y || e->length == 0) {
					op(e, MOVE_TO, l->lines[i].x0, l->lines[i].y0);
				}

				op(e, LINE_TO, l->lines[i].x1, l->lines[i].y1);
			}

			if (e->cmd_idx >= 0) {
				//printf("old command: %d %d\n", e->cmd, e->length);
				e->feature->set_geometry(e->cmd_idx, 
					(e->length << CMD_BITS) |
					(e->cmd & ((1 << CMD_BITS) - 1)));
			}
		}
	}

	//////////////////////////////////

	e->layer = e->tile.add_layers();
	e->layer->set_name("points");
	e->layer->set_version(1);
	e->layer->set_extent(XMAX);

	e->layer->add_keys("meta", strlen("meta"));
	vals = -1;

	struct metapointlayer *mpl;
	for (mpl = e->metapointlayers; mpl != NULL; mpl = mpl->next) {
		mapnik::vector::tile_value *tv = e->layer->add_values();
		tv->set_int_value(mpl->meta);
		vals++;

		struct pointlayer *p;
		for (p = mpl->pointlayers; p != NULL; p = p->next) {
			if (p->npoints != 0) {
				e->feature = e->layer->add_features();
				e->feature->set_type(mapnik::vector::tile::LineString);

				e->feature->add_tags(0); /* key, "meta" */
				e->feature->add_tags(vals); /* value */

				e->x = 0;
				e->y = 0;

				e->cmd_idx = -1;
				e->cmd = -1;
				e->length = 0;

				for (i = 0; i < p->npoints; i++) {
					op(e, MOVE_TO, p->points[i].x, p->points[i].y);
					op(e, LINE_TO, p->points[i].x + 1, p->points[i].y);
				}

				if (e->cmd_idx >= 0) {
					e->feature->set_geometry(e->cmd_idx, 
						(e->length << CMD_BITS) |
						(e->cmd & ((1 << CMD_BITS) - 1)));
				}
			}
		}
	}

	//////////////////////////////////

	std::string s;
	e->tile.SerializeToString(&s);

	std::string compressed;
	compress(s, compressed);

	std::cout << compressed;
}

static void op(env *e, int cmd, int x, int y) {
	// printf("%d %d,%d\n", cmd, x, y);
	// printf("from cmd %d to %d\n", e->cmd, cmd);

	if (cmd != e->cmd) {
		if (e->cmd_idx >= 0) {
			// printf("old command: %d %d\n", e->cmd, e->length);
			e->feature->set_geometry(e->cmd_idx, 
				(e->length << CMD_BITS) |
				(e->cmd & ((1 << CMD_BITS) - 1)));
		}

		e->cmd = cmd;
		e->length = 0;
		e->cmd_idx = e->feature->geometry_size();

		e->feature->add_geometry(0); // placeholder
	}

	if (cmd == MOVE_TO || cmd == LINE_TO) {
		int dx = x - e->x;
		int dy = y - e->y;
		// printf("new geom: %d %d\n", x, y);

		e->feature->add_geometry((dx << 1) ^ (dx >> 31));
		e->feature->add_geometry((dy << 1) ^ (dy >> 31));
		
		e->x = x;
		e->y = y;
		e->length++;
	} else if (cmd == CLOSE_PATH) {
		e->length++;
	}
}

// http://rosettacode.org/wiki/Bitmap/Bresenham's_line_algorithm#C
int lineused(struct linelayer *l, int x0, int y0, int x1, int y1) {
	int dx = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
	int dy = abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
	int err = ((dx > dy) ? dx : -dy) / 2, e2;

	while (1) {
		if (x0 == x1 && y0 == y1) {
			break;
		}

		if (x0 >= 0 && y0 >=0 && x0 < 256 && y0 < 256) {
			if (l->used[y0 * 256 + x0]) {
				return 1;
			}
		}

		e2 = err;
		if (e2 > -dx) {
			err -= dy;
			x0 += sx;
		}
		if (e2 <  dy) {
			err += dx;
			y0 += sy;
		}
	}

	return 0;
}

// http://rosettacode.org/wiki/Bitmap/Bresenham's_line_algorithm#C
void useline(struct linelayer *l, int x0, int y0, int x1, int y1) {
	int dx = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
	int dy = abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
	int err = ((dx > dy) ? dx : -dy) / 2, e2;

	while (1) {
		if (x0 == x1 && y0 == y1) {
			break;
		}

		if (x0 >= 0 && y0 >=0 && x0 < 256 && y0 < 256) {
			l->used[y0 * 256 + x0] = 1;
		}

		e2 = err;
		if (e2 > -dx) {
			err -= dy;
			x0 += sx;
		}
		if (e2 <  dy) {
			err += dx;
			y0 += sy;
		}
	}
}

int drawClip(double x0, double y0, double x1, double y1, struct graphics *gc, double bright, double hue, long long meta, int antialias, double thick, struct tilecontext *tc) {
	double mult = XMAX / gc->width;
	int accept = clip(&x0, &y0, &x1, &y1, -1, -1, XMAX / mult + 1, YMAX / mult + 1);

	if (accept) {
		int xx0 = x0 * mult;
		int yy0 = y0 * mult;
		int xx1 = x1 * mult;
		int yy1 = y1 * mult;

		env *e = gc->e;
		struct metalinelayer **mll = &(e->metalinelayers);
		while (*mll != NULL) {
			if ((*mll)->meta == meta) {
				break;
			}

			mll = &((*mll)->next);
		}
		if (*mll == NULL) {
			*mll = new_metalinelayer(meta, *mll);
		}

		if ((*mll)->linelayers == NULL) {
			(*mll)->linelayers = new_linelayer();
		}

		struct linelayer *l = (*mll)->linelayers;

		if (xx0 != xx1 || yy0 != yy1) {
			while (l->nlines > MAX_POINTS || lineused(l, x0, y0, x1, y1)) {
				if (l->next == NULL) {
					l->next = new_linelayer();
				}
				l = l->next;
			}

			if (l->nlines + 1 >= l->nlalloc) {
				l->nlalloc *= 2;
				l->lines = (struct line *) realloc((void *) l->lines, l->nlalloc * sizeof(struct line));
			}

			useline(l, x0, y0, x1, y1);

			l->lines[l->nlines].x0 = xx0;
			l->lines[l->nlines].y0 = yy0;
			l->lines[l->nlines].x1 = xx1;
			l->lines[l->nlines].y1 = yy1;

			l->nlines++;
		}
	}

	return 0;
}

void drawPixel(double x, double y, struct graphics *gc, double bright, double hue, long long meta, struct tilecontext *tc) {
	x += .5;
	y += .5;

	double mult = XMAX / gc->width;
	int xx = x * mult;
	int yy = y * mult;

	env *e = gc->e;

	int xu = x * 256 / gc->width;
	int yu = y * 256 / gc->height;
	if (xu < 0) {
		xu = 0;
	}
	if (xu > 255) {
		xu = 255;
	}
	if (yu < 0) {
		yu = 0;
	}
	if (yu > 255) {
		yu = 255;
	}

	struct metapointlayer **mpl = &e->metapointlayers;
	while (*mpl != NULL) {
		if ((*mpl)->meta == meta) {
			break;
		}

		mpl = &((*mpl)->next);
	}
	if (*mpl == NULL) {
		*mpl = new_metapointlayer(meta, *mpl);
	}

	struct pointlayer *p = (*mpl)->used[256 * yu + xu];
	while (p->npoints >= MAX_POINTS) {
		if (p->next == NULL) {
			p->next = new_pointlayer();
		}
		p = p->next;
	}
	if (p->next == NULL) {
		p->next = new_pointlayer();
	}
	(*mpl)->used[256 * yu + xu] = p->next;

	if (p->npoints + 1 >= p->npalloc) {
		p->npalloc *= 2;
		p->points = (struct point *) realloc((void *) p->points, p->npalloc * sizeof(struct point));
	}

	p->points[p->npoints].x = xx;
	p->points[p->npoints].y = yy;

	p->npoints++;
}

void drawBrush(double x, double y, struct graphics *gc, double bright, double brush, double hue, long long meta, int gaussian, struct tilecontext *tc) {
	drawPixel(x - .5, y - .5, gc, bright, hue, meta, tc);
}

void setClip(struct graphics *gc, int x, int y, int w, int h) {

}

