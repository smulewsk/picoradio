#pragma once
/* Setup access point: DHCP + captive DNS on the AP interface (192.168.4.1) */
void ap_servers_start(void);
int  ap_client_count(void);   /* phones / laptops joined to the setup network */
