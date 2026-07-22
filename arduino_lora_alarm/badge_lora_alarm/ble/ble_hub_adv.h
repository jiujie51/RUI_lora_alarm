/* Hub BLE 广播 — RUI3 api.ble.beacon 自定义数据 */
#ifndef BLE_HUB_ADV_H
#define BLE_HUB_ADV_H

int  ble_hub_adv_start(void);
int  ble_hub_adv_stop(void);
void ble_hub_adv_update_data(void);

#endif
