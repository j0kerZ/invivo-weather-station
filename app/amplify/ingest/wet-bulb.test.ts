import { describe, expect, it } from 'vitest';
import { wetBulb } from './wet-bulb';

describe('wetBulb', () => {
  it('matches the published value for 20 °C and 50 %RH', () => {
    /* NCAR Command Language wetbulb_stull example: 13.69934 °C. */
    expect(wetBulb(20, 50)).toBeCloseTo(13.69934, 4);
  });

  it('is below the air temperature when the air is not saturated', () => {
    expect(wetBulb(2, 60)!).toBeLessThan(2);
  });

  it.each([
    [-20.01, 50],
    [50.01, 50],
    [10, 4.99],
    [10, 99.01],
  ])('gives no value outside the fitted range (%s °C, %s %RH)', (t, rh) => {
    expect(wetBulb(t, rh)).toBeUndefined();
  });

  it('gives a value at the edges of the fitted range', () => {
    expect(wetBulb(-20, 5)).toBeTypeOf('number');
    expect(wetBulb(50, 99)).toBeTypeOf('number');
  });
});
