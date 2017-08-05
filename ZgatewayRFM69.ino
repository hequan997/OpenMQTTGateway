/*  
  OpenMQTTGateway  - ESP8266 or Arduino program for home automation 

   Act as a wifi or ethernet gateway between your 433mhz/infrared IR signal  and a MQTT broker 
   Send and receiving command by MQTT
 
  This gateway enables to:
 - receive MQTT data from a topic and send RF 433Mhz signal corresponding to the received MQTT data with an RFM69 module
 - publish MQTT data to a different topic related to received 433Mhz signal from an RFM69 module

    Copyright: (c)Florian ROBERT, Felix Rusu LowPowerLab.com
    Library and code by Felix Rusu - felix@lowpowerlab.com
    Modification of the code nanohab from bbx10 https://github.com/bbx10/nanohab
  
    This file is part of OpenMQTTGateway.
    
    OpenMQTTGateway is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenMQTTGateway is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef ZgatewayRFM69

#include <RFM69.h>                //https://www.github.com/lowpowerlab/rfm69
#include <pgmspace.h>

char RadioConfig[128];

// Default values
const char PROGMEM ENCRYPTKEY[] = "sampleEncryptKey";
const char PROGMEM MDNS_NAME[] = "rfm69gw1";
const char PROGMEM MQTT_BROKER[] = "raspi2";
const char PROGMEM RFM69AP_NAME[] = "RFM69-AP";
#define NETWORKID     200  //the same on all nodes that talk to each other
#define NODEID        10

//Match frequency to the hardware version of the radio
#define FREQUENCY     RF69_433MHZ
//#define FREQUENCY     RF69_868MHZ
//#define FREQUENCY      RF69_915MHZ
#define IS_RFM69HCW    true // set to 'true' if you are using an RFM69HCW module
#define POWER_LEVEL    31

// vvvvvvvvv Global Configuration vvvvvvvvvvv
#include <EEPROM.h>

struct _GLOBAL_CONFIG {
  uint32_t    checksum;
  char        rfmapname[32];
  char        encryptkey[16+1];
  uint8_t     networkid;
  uint8_t     nodeid;
  uint8_t     powerlevel; // bits 0..4 power leve, bit 7 RFM69HCW 1=true
  uint8_t     rfmfrequency;
};

#define GC_POWER_LEVEL    (pGC->powerlevel & 0x1F)
#define GC_IS_RFM69HCW  ((pGC->powerlevel & 0x80) != 0)

struct _GLOBAL_CONFIG *pGC;


// vvvvvvvvv Global Configuration vvvvvvvvvvv
uint32_t gc_checksum() {
  uint8_t *p = (uint8_t *)pGC;
  uint32_t checksum = 0;
  p += sizeof(pGC->checksum);
  for (size_t i = 0; i < (sizeof(*pGC) - 4); i++) {
    checksum += *p++;
  }
  return checksum;
}

void eeprom_setup() {
  EEPROM.begin(4096);
  pGC = (struct _GLOBAL_CONFIG *)EEPROM.getDataPtr();
  // if checksum bad init GC else use GC values
  if (gc_checksum() != pGC->checksum) {
    trc("Factory reset");
    memset(pGC, 0, sizeof(*pGC));
    strcpy_P(pGC->encryptkey, ENCRYPTKEY);
    strcpy_P(pGC->rfmapname, RFM69AP_NAME);
    pGC->networkid = NETWORKID;
    pGC->nodeid = NODEID;
    pGC->powerlevel = ((IS_RFM69HCW)?0x80:0x00) | POWER_LEVEL;
    pGC->rfmfrequency = FREQUENCY;
    pGC->checksum = gc_checksum();
    EEPROM.commit();
  }
}

#define SELECTED_FREQ(f)  ((pGC->rfmfrequency==f)?"selected":"")

// vvvvvvvvv RFM69 vvvvvvvvvvv
#include <RFM69.h>                //https://www.github.com/lowpowerlab/rfm69
#include <SPI.h>

// ESP8266
#define RFM69_CS      D0  // GPIO15/HCS/D8
#define RFM69_IRQ     D8   // GPIO04/D2
#define RFM69_IRQN    digitalPinToInterrupt(RFM69_IRQ)
#define RFM69_RST     D4   // GPIO02/D4

RFM69 radio;

void setupRFM69(void) {
  eeprom_setup();
  
  int freq;
  static const char PROGMEM JSONtemplate[] =
    R"({"msgType":"config","freq":%d,"rfm69hcw":%d,"netid":%d,"power":%d})";
  char payload[128];

  radio = RFM69(RFM69_CS, RFM69_IRQ, GC_IS_RFM69HCW, RFM69_IRQN);
  // Hard Reset the RFM module
  pinMode(RFM69_RST, OUTPUT);
  digitalWrite(RFM69_RST, HIGH);
  delay(100);
  digitalWrite(RFM69_RST, LOW);
  delay(100);

  // Initialize radio
  if (!radio.initialize(pGC->rfmfrequency, pGC->nodeid, pGC->networkid))
  {
    trc(F("RFM69 initialization failed"));
    }
    
  if (GC_IS_RFM69HCW) {
    radio.setHighPower();    // Only for RFM69HCW & HW!
  }
  radio.setPowerLevel(GC_POWER_LEVEL); // power output ranges from 0 (5dBm) to 31 (20dBm)

  if (pGC->encryptkey[0] != '\0') radio.encrypt(pGC->encryptkey);

  trc(F("RFM69 Listening and transmitting at"));
  switch (pGC->rfmfrequency) {
    case RF69_433MHZ:
      freq = 433;
      break;
    case RF69_868MHZ:
      freq = 868;
      break;
    case RF69_915MHZ:
      freq = 915;
      break;
    case RF69_315MHZ:
      freq = 315;
      break;
    default:
      freq = -1;
      break;
  }
  trc(String(freq));

  size_t len = snprintf_P(RadioConfig, sizeof(RadioConfig), JSONtemplate,
      freq, GC_IS_RFM69HCW, pGC->networkid, GC_POWER_LEVEL);
  if (len >= sizeof(RadioConfig)) {
    trc("\n\n*** RFM69 config truncated ***\n");
  }
}

boolean RFM69toMQTT(void) {
  //check if something was received (could be an interrupt from the radio)
  if (radio.receiveDone())
  {
    uint8_t senderId;
    int16_t rssi;
    uint8_t data[RF69_MAX_DATA_LEN];

    //save packet because it may be overwritten
    senderId = radio.SENDERID;
    rssi = radio.readRSSI(false);
    memcpy(data, (void *)radio.DATA, radio.DATALEN);
  
    client.publish(subjectRFM69toMQTT,(char *)data);
    //client.publish(subjectRFM69toMQTTrssi,(char *)rssi);
    //client.publish(subjectRFM69toMQTTsender,senderId);
    trc(F("Data received"));
    trc((const char *)data);

    //check if sender wanted an ACK
    if (radio.ACKRequested())
    {
      radio.sendACK();
    }
    radio.receiveDone(); //put radio in RX mode
    //updateClients(senderId, rssi, (const char *)data);

    return true;
    
  } else {
    radio.receiveDone(); //put radio in RX mode
    return false;
  }
}

boolean MQTTtoRFM69(char * topicOri, char * datacallback) {
  int loops;
  uint32_t startMillis;
  static uint32_t deltaMillis = 0;
  // RF DATA ANALYSIS
  //We look into the subject to see if a special RF protocol is defined 
  String topic = topicOri;
  int valueRCV = 1; //default receiver id value
  int pos = topic.lastIndexOf(RFM69receiverKey);       
  if (pos != -1){
    pos = pos + +strlen(RFM69receiverKey);
    valueRCV = (topic.substring(pos,pos + 3)).toInt();
    trc(F("RFM69 receiver ID:"));
    trc(String(valueRCV));
  }
if ((topic == subjectMQTTtoRFM69) && (valueRCV == 1)){
    trc(F("MQTTtoRFM69 default"));
  loops = 10;
  startMillis = millis();
  while (loops--) {
    if(radio.sendWithRetry(valueRCV, datacallback, strlen(datacallback)+1)) {
      deltaMillis = millis() - startMillis;
      Serial.print(" OK ");
      Serial.println(deltaMillis);
      // Acknowledgement to the GTWRF topic
    boolean result = client.publish(subjectGTWRFM69toMQTT, datacallback);
    if (result)trc(F("Ack published"));
    return true;
      break;
    }
    else {
      Serial.print("!");
    }
    delay(50);
  }
  if (loops <= 0) {
    deltaMillis = 0;
   trc(F("RFM69 sending failed"));
      return false;
  }
  } else if (valueRCV != 1) {
    trc(F("MQTTtoRFM69 user parameters"));
    loops = 10;
  startMillis = millis();
  while (loops--) {
    if(radio.sendWithRetry(valueRCV, datacallback, strlen(datacallback)+1)) {
      deltaMillis = millis() - startMillis;
      Serial.print(" OK ");
      Serial.println(deltaMillis);
      // Acknowledgement to the GTWRF topic
    boolean result = client.publish(subjectGTWRFM69toMQTT, datacallback);
    if (result)trc(F("Ack published"));
    return true;
      break;
    }
    else {
      Serial.print("!");
    }
    delay(50);
  }
  if (loops <= 0) {
    deltaMillis = 0;
   trc(F("RFM69 sending failed"));
      return false;
  }
    }
}
#endif