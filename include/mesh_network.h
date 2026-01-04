#include <string.h>
#include <inttypes.h>

#include "esp_mesh.h"
#include "esp_event.h"
#include "nvs_flash.h"


int init_wifi(void);
int init_mesh(void);
int send(mesh_addr_t *address, mesh_data_t *data);
int recv(mesh_addr_t *address, mesh_data_t *data);