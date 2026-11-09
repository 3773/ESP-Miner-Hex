#ifndef ADC_H_
#define ADC_H_

void ADC_init(void);
uint16_t ADC_get_vcore(void);

// bitaxeHex: the ESP32 samples the top of voltage domain 1 (VDD1, GPIO14 =
// ADC2 ch3) and domain 2 (VDD2, GPIO12 = ADC2 ch1) through 7.87k/22.1k dividers.
void ADC_init_hex_domains(void);
// returns VDD1 / VDD2 in mV (divider already removed), -1 if not readable.
// Only Rev 304 has these dividers; on 302/303 GPIO12/14 are test points.
int ADC_get_hex_vdd_mv(int domain);
// bitaxeHex (all revisions): top of the core stack in mV from GPIO2, -1 if not readable
int ADC_get_stack_mv(void);

#endif /* ADC_H_ */