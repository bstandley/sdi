#define LAN
#define MSGLEN 64  // includes null-terminator
#define PORT   18

#include "eeprom/shared.h"  // includes EEPROM.h
#include "comm.h"           // includes Ethernet.h, if enabled
#include "parse.h"

const unsigned long conf_commit             = 0x1234abc;  // edit to match current commit before compile/download!
const long          conf_clock_freq_int     = 2000000;    // board-dependent, assumes prescaler set to /8
const byte          conf_clock_pin          = 12;         // board-dependent
const byte          conf_trig_pin           = 2;          // pin must support low-level interrupts
const unsigned int  conf_start_us           = 10;
const unsigned int  conf_dhcp_cycles        = 1000;
const unsigned long conf_measure_ms         = 500;
const byte          conf_pulse_pin  [NCHAN] = {8, 7, 6, 5};
byte *              conf_pulse_port [NCHAN] = {&PORTB, &PORTE, &PORTD, &PORTC};  // board-dependent, must match pulse_pin
const byte          conf_pulse_mask [NCHAN] = {1 << 4, 1 << 6, 1 << 7, 1 << 6};  // board dependent, must match pulse_pin

// SCPI commands:
//   *IDN?                 model and version
//   *TRG                  soft trigger, independent of :TRIG:ARMed
//   *SAV                  save settings to EEPROM (LAN excluded)
//   *RCL                  recall EEPROM settings (also performed on startup) (LAN excluded)
//   *RST                  reset to default settings (LAN excluded)
//   :CLOCK:FREQ:INTernal  ideal internal frequency in Hz
//   :CLOCK:FREQ:MEASure   measured frequency in Hz of currently-configured clock
//   :LAN:IP               current ip address

// persistent SCPI settings:
SCPI scpi;

// runtime SCPI variables (read-only except where noted):
long          scpi_clock_freq;          // :CLOCK:FREQ      ideal frequency in Hz of currently-configure clock
volatile long scpi_trig_count;          // :TRIG:COUNT      hardware triggers detected since reboot
volatile bool scpi_trig_armed;          // :TRIG:ARMed      armed (read/write)
volatile bool scpi_trig_ready;          // :TRIG:READY      ready (armed plus at least one valid channel)
bool          scpi_pulse_valid[NCHAN];  // :PULSe<n>:VALid  output channel has valid/usable pulse sequence
#ifdef LAN
byte          scpi_lan_mode;            // :LAN:MODE        actual mode (writes go to scpi_lan.mode)
#endif

unsigned long          k_delay  [NCHAN];
unsigned long          k_width  [NCHAN];
unsigned long          k_period [NCHAN];
unsigned long          k_end    [NCHAN];
volatile unsigned long k_next   [NCHAN];
volatile bool          x_next   [NCHAN];
volatile int           N_active;
volatile unsigned long k_cur;
volatile unsigned int  c_cur;

#ifdef LAN
EthernetServer server(PORT);
unsigned int countdown_dhcp;
#endif

void setup()
{
    EEPROM.get(EPA_SCPI, scpi);

    pinMode(conf_clock_pin, INPUT);
    update_clock();  // also initialize scpi_clock_freq

    for (int n = 0; n < NCHAN; n++)
    {
        pinMode(conf_pulse_pin[n], OUTPUT);
        update_pulse(n);  // also initialize scpi_pulse_valid[n]
    }

    pinMode(conf_trig_pin, INPUT);
    scpi_trig_count = 0;
    scpi_trig_armed = scpi.trig_rearm;
    update_trig_ready();  // also initialize scpi_trig_ready
    update_trig_edge();  // actually configure interrupt

    Serial.begin(9600);

#ifdef LAN
    SCPI_LAN scpi_lan;
    EEPROM.get(EPA_SCPI_LAN, scpi_lan);

    scpi_lan_mode = scpi_lan.mode;
    if (scpi_lan_mode == LAN_DHCP)
    {
        Ethernet.begin(scpi_lan.mac);
        countdown_dhcp = conf_dhcp_cycles;
    }
    else if (scpi_lan_mode == LAN_STATIC)
    {
        const byte dns[4] = {0, 0, 0, 0};  // DNS is not used
        Ethernet.begin(scpi_lan.mac, scpi_lan.ip_static, dns, scpi_lan.gateway_static, scpi_lan.subnet_static);
    }
#endif
}

void loop()
{
    if (Serial.available() > 0)
    {
        lan = 0;  // receive/send via Serial
        char msg[MSGLEN];
        if (recv_msg(msg)) { parse_msg(msg); }
    }

#ifdef LAN
    if (scpi_lan_mode != LAN_OFF)
    {
        client = server.available();
        if (client)
        {
            lan = 1;  // receive/send via client
            char msg[MSGLEN];
            if (recv_msg(msg)) { parse_msg(msg); }
        }
    }

    if (scpi_lan_mode == LAN_DHCP)
    {
        if (countdown_dhcp == 0)
        {
            Ethernet.maintain();
            countdown_dhcp = conf_dhcp_cycles;
        }
        else { countdown_dhcp--; }
    }
#endif

    delay(1);
}

void run_null() { return; }

void run_hw_trig()
{
    c_cur = TCNT1;        // TESTING: read as early as possible to make global timebase as accurate as possible
    if (scpi_trig_ready)  // TESTING: precalculated to save a bit of time
    {
        gen_pulses();
        scpi_trig_armed = scpi.trig_rearm;
        update_trig_ready();
    }
    scpi_trig_count++;
}

void run_sw_trig()  // subset of run_hw_trig() ignoring armed/disarmed and trigger count
{
    c_cur = TCNT1;
    if (N_active > 0)
    {
        gen_pulses();
        update_trig_ready();
    }
}

void gen_pulses()
{
    for (int n = 0; n < NCHAN; n++)  // TESTING: quick iteration to accelerate short delays
    {
        if (k_cur >= k_next[n])  // use x_next[n] == 1 below:
        {
            pulse_write(n, !scpi.pulse_invert[n]);

            k_next[n] += k_width[n];
            x_next[n] = 0;
        }
    }
    
    scpi_trig_ready = 0;  // ok to set now  TODO: test against spurious triggers

    while (1)
    {
        unsigned int c_diff = TCNT1 - c_cur;
        c_cur += c_diff;
        k_cur += c_diff;

        for (int n = 0; n < NCHAN; n++)  // full loop
        {
            if (k_cur >= k_next[n])
            {
                pulse_write(n, x_next[n] ? !scpi.pulse_invert[n] : scpi.pulse_invert[n]);

                k_next[n] += x_next[n] ? k_width[n] : (k_period[n] - k_width[n]);
                x_next[n] = !x_next[n];

                if (k_next[n] >= k_end[n])
                {
                    pulse_write(n, scpi.pulse_invert[n]);
                    k_next[n] = 4100000000;
                    N_active--;
                    if (N_active == 0) { return; }
                }
            }
        }
    }
}

void pulse_write(const int n, const bool x)  // TESTING: a bit quicker than digitalWrite()
{
    if (x) { *conf_pulse_port[n] |=  conf_pulse_mask[n]; }
    else   { *conf_pulse_port[n] &= ~conf_pulse_mask[n]; }
}

long measure_freq()
{
    unsigned long t = millis();
    unsigned int  c = TCNT1;
    long          k = 0;

    while (millis() - t <= conf_measure_ms)
    {
        unsigned int c_diff = TCNT1 - c;
        c += c_diff;
        k += c_diff;
    }

    return (1000 * k) / conf_measure_ms;
}

// runtime update functions:

bool update_clock()
{
    scpi_clock_freq = (scpi.clock_src == INTERNAL) ? conf_clock_freq_int : scpi.clock_freq_ext;

    TCCR1A = 0x0;                                  // COM1A1=0 COM1A0=0 COM1B1=0 COM1B0=0 FOC1A=0 FOC1B=0 WMG11=0 WGM10=0
    TCCR1B = (scpi.clock_src == INTERNAL) ? 0x2 :  // ICNC1=0  ICES1=0  n/a=0    WGM13=0  WGM12=0 CS12=0  CS11=1  CS10=0  (internal, /8)
                                            0x7;   // ICNC1=0  ICES1=0  n/a=0    WGM13=0  WGM12=0 CS12=1  CS11=1  CS10=1  (external rising)
}

void update_trig_edge()
{
    attachInterrupt(digitalPinToInterrupt(conf_trig_pin), run_null,    scpi.trig_edge);
    delay(100);  // let possibly lingering interrupt clear out
    attachInterrupt(digitalPinToInterrupt(conf_trig_pin), run_hw_trig, scpi.trig_edge);
}

bool update_pulse(const int n)
{
    pulse_write(n, scpi.pulse_invert[n]);  // if inverting, then set initial value HIGH	

    float scpi_clock_freq_MHz = float(scpi_clock_freq) / 1e6;

    k_delay[n]  = scpi_clock_freq_MHz * scpi.pulse_delay[n];
    k_width[n]  = scpi_clock_freq_MHz * min(scpi.pulse_width[n], scpi.pulse_period[n]);
    k_period[n] = scpi_clock_freq_MHz * scpi.pulse_period[n];
    k_end[n]    = k_delay[n] + k_period[n] * scpi.pulse_cycles[n];

    return scpi_pulse_valid[n] = (scpi_clock_freq_MHz * (float(scpi.pulse_delay[n]) + float(scpi.pulse_period[n]) * scpi.pulse_cycles[n]) < 4e9);  // calculate with floats
}

bool update_pulse_all()
{
    bool rv = 1;
    for (int n = 0; n < NCHAN; n++) { if (!update_pulse(n)) { rv = 0; } }
    return rv;
}

void update_trig_ready()
{
    k_cur = (scpi_clock_freq * conf_start_us) / 1000000;  // account for interrupt processing time
    N_active = 0;

    for (int n = 0; n < NCHAN; n++)
    {
        if (scpi_pulse_valid[n] && (scpi.pulse_cycles[n] > 0))
        {
            N_active++;
            k_next[n] = k_delay[n];
            x_next[n] = 1;
        }
        else { k_next[n] = 4100000000; }
    }

    scpi_trig_ready = scpi_trig_armed && N_active > 0;
}

// SCPI parsing functions:

void parse_msg(const char *msg)
{
    char rest[MSGLEN];
    bool update = 0;

    if (equal(msg, "*IDN?"))
    {
        unsigned long eeprom_commit;
        EEPROM.get(EPA_COMMIT, eeprom_commit);

        send_eps(EPA_IDN,       NOEOL);
        send_str(" (PROG: ",    NOEOL);
        send_hex(conf_commit,   NOEOL);
        send_str(", EEPROM: ",  NOEOL);
        send_hex(eeprom_commit, NOEOL);
        send_str(")");
    }
    else if (equal(msg, "*TRG"))                                                     { run_sw_trig();              send_str("OK"); }
    else if (equal(msg, "*SAV"))                                                     { EEPROM.put(EPA_SCPI, scpi); send_str("OK"); }
    else if (equal(msg, "*RCL"))                                                     { EEPROM.get(EPA_SCPI, scpi); update = 1;     }
    else if (equal(msg, "*RST"))                                                     { scpi_default(scpi);         update = 1;     }
    else if (start(msg, ":CLOCK:",                                            rest)) { parse_clock(rest);                          }
    else if (start(msg, ":TRIG:",                                             rest)) { parse_trig(rest);                           }
    else if (start(msg, ":PULSE1:", ":PULS1:",                                rest)) { parse_pulse(0, rest);                       }
    else if (start(msg, ":PULSE2:", ":PULS2:",                                rest)) { parse_pulse(1, rest);                       }
    else if (start(msg, ":PULSE3:", ":PULS3:",                                rest)) { parse_pulse(2, rest);                       }
    else if (start(msg, ":PULSE4:", ":PULS4:",                                rest)) { parse_pulse(3, rest);                       }
    else if (start(msg, ":SYSTEM:COMMUNICATE:LAN:", ":SYST:COMMUNICATE:LAN:", rest)
          || start(msg, ":SYSTEM:COMM:LAN:",        ":SYST:COMM:LAN:",        rest)
          || start(msg, ":LAN:",                                              rest)) { parse_lan(rest);                            }
    else                                                                             { send_eps(EPA_REPLY_INVALID_CMD);            }

    if (update)
    {
        update_clock();
        bool ok = update_pulse_all();  // at least one channel might be not ok . . .
        update_trig_ready();
        update_trig_edge();

        if (ok) { send_str("OK");            }
        else    { send_eps(EPA_REPLY_CHECK); }
        
    }
}

void parse_clock(const char *rest)
{
    char arg[MSGLEN];
    bool update = 0;

    if      (equal(rest, "SRC?"))                               { send_str(scpi.clock_src == INTERNAL ? "INTERNAL" : "EXTERNAL"); }
    else if (start(rest, "SRC ", arg))
    {
        if      (equal(arg, "INTERNAL", "INT"))                 { scpi.clock_src = INTERNAL; update = 1;                          }
        else if (equal(arg, "EXTERNAL", "EXT"))                 { scpi.clock_src = EXTERNAL; update = 1;                          }
        else                                                    { send_eps(EPA_REPLY_INVALID_ARG);                                }

    }
    else if (equal(rest, "FREQ?"))                              { send_int(scpi_clock_freq);                                      }
    else if (start(rest, "FREQ ",                        NULL)) { send_eps(EPA_REPLY_READONLY);                                   }
    else if (equal(rest, "FREQ:MEASURE?",  "FREQ:MEAS?"))       { send_int(measure_freq());                                       }
    else if (start(rest, "FREQ:MEASURE ",  "FREQ:MEAS ", NULL)) { send_eps(EPA_REPLY_READONLY);                                   }
    else if (equal(rest, "FREQ:INTERNAL?", "FREQ:INT?"))        { send_int(conf_clock_freq_int);                                  }
    else if (start(rest, "FREQ:INTERNAL ", "FREQ:INT ",  NULL)) { send_eps(EPA_REPLY_READONLY);                                   }
    else if (equal(rest, "FREQ:EXTERNAL?", "FREQ:EXT?"))        { send_int(scpi.clock_freq_ext);                                  }
    else if (start(rest, "FREQ:EXTERNAL ", "FREQ:EXT ",  arg))
    {
        if (parse_num(arg, scpi.clock_freq_ext, ZERO_NOK))      { update = 1;                                                     }
        else                                                    { send_eps(EPA_REPLY_INVALID_ARG);                                }
    }
    else                                                        { send_eps(EPA_REPLY_INVALID_CMD);                                }

    if (update)
    {
        update_clock();
        bool ok = update_pulse_all();  // at least one channel might be not ok . . .
        update_trig_ready();

        if (ok) { send_str("OK");            }
        else    { send_eps(EPA_REPLY_CHECK); }
    }
}

void parse_trig(const char *rest)
{
    char arg[MSGLEN];
    bool update = 0;

    if      (equal(rest, "EDGE?"))               { send_str(scpi.trig_edge == RISING ? "RISING" : "FALLING"); }
    else if (start(rest, "EDGE ", arg))
    {
        if      (equal(arg, "RISING",  "RIS"))   { scpi.trig_edge = RISING;  update = 1;                      }
        else if (equal(arg, "FALLING", "FALL"))  { scpi.trig_edge = FALLING; update = 1;                      }
        else                                     { send_eps(EPA_REPLY_INVALID_ARG);                           }
    }
    else if (equal(rest, "ARMED?", "ARM?"))      { send_hex(scpi_trig_armed);                                 }
    else if (start(rest, "ARMED ", "ARM ", arg))
    {
        if      (equal(arg, "1"))                { scpi_trig_armed = 1; update = 1;                           }
        else if (equal(arg, "0"))                { scpi_trig_armed = 0; update = 1;                           }
        else                                     { send_eps(EPA_REPLY_INVALID_ARG);                           }

    }
    else if (equal(rest, "READY?"))              { send_hex(scpi_trig_ready);                                 }
    else if (start(rest, "READY ", NULL))        { send_eps(EPA_REPLY_READONLY);                              }
    else if (equal(rest, "REARM?"))              { send_hex(scpi.trig_rearm);                                 }
    else if (start(rest, "REARM ", arg))
    {
        if      (equal(arg, "1"))                { scpi.trig_rearm = 1; send_str("OK");                       }
        else if (equal(arg, "0"))                { scpi.trig_rearm = 0; send_str("OK");                       }
        else                                     { send_eps(EPA_REPLY_INVALID_ARG);                           }
    }
    else if (equal(rest, "COUNT?"))              { send_int(scpi_trig_count);                                 }
    else if (start(rest, "COUNT ", NULL))        { send_eps(EPA_REPLY_READONLY);                              }
    else                                         { send_eps(EPA_REPLY_INVALID_CMD);                           }

    if (update)
    {
        update_trig_ready();  // always ok
        update_trig_edge();   // always ok
        send_str("OK");
    }
}

void parse_pulse(const int n, const char *rest)
{
    char arg[MSGLEN];
    bool update = 0;

    if      (equal(rest, "DELAY?", "DEL?"))                    { send_micros(scpi.pulse_delay[n]);     }
    else if (start(rest, "DELAY ", "DEL ",  arg))
    {
        if (parse_micros(arg, scpi.pulse_delay[n], ZERO_OK))   { update = 1;                           }
        else                                                   { send_eps(EPA_REPLY_INVALID_ARG);      }
    }
    else if (equal(rest, "WIDTH?", "WID?"))                    { send_micros(scpi.pulse_width[n]);     }
    else if (start(rest, "WIDTH ", "WID ",  arg))
    {
        if (parse_micros(arg, scpi.pulse_width[n], ZERO_NOK))  { update = 1;                           }
        else                                                   { send_eps(EPA_REPLY_INVALID_ARG);      }
    }
    else if (equal(rest, "PERIOD?", "PER?"))                   { send_micros(scpi.pulse_period[n]);    }
    else if (start(rest, "PERIOD ", "PER ", arg))
    {
        if (parse_micros(arg, scpi.pulse_period[n], ZERO_NOK)) { update = 1;                           }
        else                                                   { send_eps(EPA_REPLY_INVALID_ARG);      }
    }
    else if (equal(rest, "CYCLES?", "CYC?"))                   { send_int(scpi.pulse_cycles[n]);       }
    else if (start(rest, "CYCLES ", "CYC ", arg))
    {
        if (parse_num(arg, scpi.pulse_cycles[n], ZERO_OK))     { update = 1; }
        else                                                   { send_eps(EPA_REPLY_INVALID_ARG);      }
    }
    else if (equal(rest, "INVERT?", "INV?"))                   { send_hex(scpi.pulse_invert[n]);       }
    else if (start(rest, "INVERT ", "INV ", arg))
    {
        if      (equal(arg, "1"))                              { scpi.pulse_invert[n] = 1; update = 1; }
        else if (equal(arg, "0"))                              { scpi.pulse_invert[n] = 0; update = 1; }
        else                                                   { send_eps(EPA_REPLY_INVALID_ARG);      }
    }
    else if (equal(rest, "VALID?", "VAL?"))                    { send_hex(scpi_pulse_valid[n]);        }
    else if (start(rest, "VALID ", "VAL ", NULL))              { send_eps(EPA_REPLY_READONLY);         }
    else                                                       { send_eps(EPA_REPLY_INVALID_CMD);      }

    if (update)
    {
        bool ok = update_pulse(n);
        update_trig_ready();

        if (ok) { send_str("OK");            }
        else    { send_eps(EPA_REPLY_CHECK); }
    }
}

void parse_lan(const char *rest)
{
    char arg[MSGLEN];
    bool update = 0;

    SCPI_LAN scpi_lan;
    EEPROM.get(EPA_SCPI_LAN, scpi_lan);

    if (equal(rest, "MODE?"))
    {
        send_str(scpi_lan.mode == LAN_OFF ? "OFF" : (scpi_lan.mode == LAN_DHCP ? "DHCP" : "STATIC"), NOEOL);
        send_str(" (ACTUAL: ",                                                                       NOEOL);
#ifdef LAN
        send_str(scpi_lan_mode == LAN_OFF ? "OFF" : (scpi_lan_mode == LAN_DHCP ? "DHCP" : "STATIC"), NOEOL);
#else
        send_str("NOT AVAILABLE",                                                                    NOEOL);
#endif
        send_str(")");
    }
    else if (start(rest, "MODE ", arg))
    {
        if      (equal(arg, "OFF"))                                { scpi_lan.mode = LAN_OFF;    update = 1; }
        else if (equal(arg, "DHCP"))                               { scpi_lan.mode = LAN_DHCP;   update = 1; }
        else if (equal(arg, "STATIC", "STAT"))                     { scpi_lan.mode = LAN_STATIC; update = 1; }
        else                                                       { send_eps(EPA_REPLY_INVALID_ARG);        }

    }
    else if (equal(rest, "MAC?"))                                  { send_mac(scpi_lan.mac);                 }
    else if (start(rest, "MAC ", arg))
    {
        if (parse_mac(arg, scpi_lan.mac))                          { update = 1;                             }
        else                                                       { send_eps(EPA_REPLY_INVALID_ARG);        }
    }
    else if (equal(rest, "IP?"))
    {
        byte addr[4] = {0, 0, 0, 0};
#ifdef LAN
        if (scpi_lan_mode != LAN_OFF)
        {
            IPAddress local_ip = Ethernet.localIP();
            for (int i = 0; i < 4; i++) { addr[i] = local_ip[i]; }
        }
#endif
        send_ip(addr);
    }
    else if (start(rest, "IP ",                             NULL)) { send_eps(EPA_REPLY_READONLY);           }
    else if (equal(rest, "IP:STATIC?", "IP:STAT?"))                { send_ip(scpi_lan.ip_static);            }
    else if (start(rest, "IP:STATIC ", "IP:STAT ",          arg))
    {
        if (parse_ip(arg, scpi_lan.ip_static))                     { update = 1;                             }
        else                                                       { send_eps(EPA_REPLY_INVALID_ARG);        }
    }
    else if (equal(rest, "GATEWAY:STATIC?", "GATE:STATIC?")
          || equal(rest, "GATEWAY:STAT?",   "GATE:STAT?"))         { send_ip(scpi_lan.gateway_static);       }
    else if (start(rest, "GATEWAY:STATIC ", "GATE:STATIC ", arg)
          || start(rest, "GATEWAY:STAT ",   "GATE:STAT ",   arg))
    {
        if (parse_ip(arg, scpi_lan.gateway_static))                { update = 1;                             }
        else                                                       { send_eps(EPA_REPLY_INVALID_ARG);        }
    }
    else if (equal(rest, "SUBNET:STATIC?", "SUB:STATIC?")
          || equal(rest, "SUBNET:STAT?",   "SUB:STAT?"))           { send_ip(scpi_lan.subnet_static);        }
    else if (start(rest, "SUBNET:STATIC ", "SUB:STATIC ",   arg)
          || start(rest, "SUBNET:STAT ",   "SUB:STAT ",     arg))
    {
        if (parse_ip(arg, scpi_lan.subnet_static))                 { update = 1;                             }
        else                                                       { send_eps(EPA_REPLY_INVALID_ARG);        }
    }
    else                                                           { send_eps(EPA_REPLY_INVALID_CMD);        }

    if (update)
    {
        EEPROM.put(EPA_SCPI_LAN, scpi_lan);  // save immediately
        send_eps(EPA_REPLY_REBOOT);
    }
}
