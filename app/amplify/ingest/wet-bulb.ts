/*
 * Wet-bulb temperature from air temperature and relative humidity: Stull, R. (2011), "Wet-Bulb
 * Temperature from Relative Humidity and Air Temperature", J. Appl. Meteor. Climatol. 50, 2267-2269,
 * doi:10.1175/JAMC-D-11-0143.1. Fitted at sea-level pressure; errors -1 to +0.65 °C.
 */

/*
 * The range the formula was fitted for. Outside it, no value is better than a wrong one. The paper
 * also excludes situations that are both dry and cold, without a numeric boundary, so those are
 * not filtered here.
 */
const RH_MIN = 5;
const RH_MAX = 99;
const T_MIN = -20;
const T_MAX = 50;

/** °C, or undefined when the inputs are outside the formula's valid range. */
export function wetBulb(tempC: number, rhPct: number): number | undefined {
  if (tempC < T_MIN || tempC > T_MAX || rhPct < RH_MIN || rhPct > RH_MAX) return undefined;
  const t = tempC;
  const rh = rhPct;
  return (
    t * Math.atan(0.151977 * Math.sqrt(rh + 8.313659)) +
    Math.atan(t + rh) -
    Math.atan(rh - 1.676331) +
    0.00391838 * rh ** 1.5 * Math.atan(0.023101 * rh) -
    4.686035
  );
}
