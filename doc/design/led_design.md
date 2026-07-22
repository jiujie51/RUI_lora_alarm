# Additional LED Colors – Standard Response Protocol (SRP)

## Currently the system support below colors:
- Red
- Blue
- Yellow 
- Green

We need to make sure that the platform supports **RBG values** instead of defining the colors in a different way. This will enable us to satisfy the requirements for Standard Response protocol (SRP), by sending below colors directly from the dashboard (backend). These are not initiated from a badge:

| Command | Trigger Source | Broadcast Target | LED Color | Badge LCD Message |
|---------|----------------|------------------|-----------|-------------------|
| Hold    | Dashboard only | All Badges & Hubs | Purple    | Hold Alert        |
| Secure  | Dashboard only | All Badges & Hubs | Blue      | Secure Alert      |
| Evacuate| Dashboard only | All Badges & Hubs | Green     | Evacuate Alert    |
| Shelter | Dashboard only | All Badges & Hubs | Orange    | Shelter Alert     |


Make sure the LEDs can support below, making the colors on badges and Hubs (in particular) be unmistakable from a distance (better visibility) – values are slightly tuned from the typical 255 combinations:

| 颜色名称 | R    | G    | B    | 说明                  |
|----------|------|------|------|-----------------------|
| Red      | 255  | 0    | 0    | strong alert          |
| Blue     | 0    | 80   | 255  | brighter blue         |
| Yellow   | 255  | 220  | 0    | avoids green tint     |
| Green    | 0    | 255  | 60   | more visible          |
| Purple   | 180  | 0    | 255  | richer purple         |
| Orange   | 255  | 120  | 0    | clearer orange        |

Can you propose the Json code that can be sent from the Dashboard for each these colors?
## Badge LCD Size
Make the LCD screen to be able to receive a short text message from the dashboard. Eg the school principal sends a broadcast to all staff “first responders in the building”  or to one staff member “post your student status”. 