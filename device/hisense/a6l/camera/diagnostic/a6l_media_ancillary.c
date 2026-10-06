/* Read-only media topology probe. Does not set controls or start streaming. */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/media.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const char *entity_name(struct media_v2_entity *entities, unsigned int count,
                               unsigned int id)
{
        for (unsigned int i = 0; i < count; i++)
                if (entities[i].id == id) return entities[i].name;
        return "<not-an-entity>";
}

int main(void)
{
        alarm(3);
        glob_t paths = { 0 };
        if (glob("/dev/media*", 0, NULL, &paths)) {
                fprintf(stderr, "No media devices\n");
                return 2;
        }
        unsigned int total = 0;
        for (size_t i = 0; i < paths.gl_pathc; i++) {
                int fd = open(paths.gl_pathv[i], O_RDONLY | O_CLOEXEC);
                if (fd < 0) { perror(paths.gl_pathv[i]); continue; }
                struct media_v2_topology topology = { 0 };
                if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &topology)) {
                        perror("MEDIA_IOC_G_TOPOLOGY count"); close(fd); continue;
                }
                if (topology.num_entities > 4096 || topology.num_links > 16384) {
                        fprintf(stderr, "Topology exceeds bounded diagnostic capacity\n");
                        close(fd); continue;
                }
                struct media_v2_entity *entities = calloc(topology.num_entities, sizeof(*entities));
                struct media_v2_link *links = calloc(topology.num_links, sizeof(*links));
                if ((topology.num_entities && !entities) || (topology.num_links && !links)) return 3;
                topology.ptr_entities = (uintptr_t)entities;
                topology.ptr_links = (uintptr_t)links;
                /* Pads/interfaces aren't needed to identify entity-to-entity ancillary links. */
                topology.ptr_interfaces = 0;
                topology.ptr_pads = 0;
                if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &topology)) {
                        perror("MEDIA_IOC_G_TOPOLOGY data");
                } else {
                        printf("MEDIA %s version=%llu entities=%u links=%u\n", paths.gl_pathv[i],
                               (unsigned long long)topology.topology_version,
                               topology.num_entities, topology.num_links);
                        for (unsigned int n = 0; n < topology.num_entities; n++)
                                printf("ENTITY id=%u function=0x%x name=%s\n", entities[n].id,
                                       entities[n].function, entities[n].name);
                        for (unsigned int n = 0; n < topology.num_links; n++) {
                                struct media_v2_link *link = &links[n];
                                if ((link->flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_ANCILLARY_LINK)
                                        continue;
                                printf("ANCILLARY id=%u flags=0x%x source=%u(%s) sink=%u(%s)\n",
                                       link->id, link->flags, link->source_id,
                                       entity_name(entities, topology.num_entities, link->source_id),
                                       link->sink_id, entity_name(entities, topology.num_entities, link->sink_id));
                                ++total;
                        }
                }
                free(entities); free(links); close(fd);
        }
        globfree(&paths);
        printf("ANCILLARY_TOTAL %u\n", total);
        return 0;
}
