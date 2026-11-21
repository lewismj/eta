import { describe, expect, it } from 'vitest';
import { decimalsFor, formatNumber, formatValue } from './format';

describe('formatNumber', () => {
  it('formats percentages with step-driven precision', () => {
    expect(formatNumber(0.05, 'pct', 0.05)).toBe('5%');
    expect(formatNumber(0.125, 'pct', 0.005)).toBe('12.5%');
    expect(formatNumber(0.0723, 'pct')).toBe('7.23%');
  });
  it('formats basis points and integers', () => {
    expect(formatNumber(0.0025, 'bp', 0.0005)).toBe('25 bp');
    expect(formatNumber(1234.6, 'int')).toBe('1,235');
  });
  it('uses a true minus sign and never prints -0', () => {
    expect(formatNumber(-0.02, 'pct', 0.01)).toBe('−2%');
    expect(formatNumber(-1e-15, 'num', 0.1)).toBe('0.0');
  });
  it('shows a dash for missing values', () => {
    expect(formatNumber(NaN)).toBe('–');
    expect(formatNumber(Infinity)).toBe('–');
  });
  it('scales default precision with magnitude', () => {
    expect(formatNumber(1.4234)).toBe('1.42');
    expect(formatNumber(152.33)).toBe('152.3');
    expect(formatNumber(15233.4)).toBe('15,233');
  });
});

describe('decimalsFor', () => {
  it('derives decimals from a step', () => {
    expect(decimalsFor(1)).toBe(0);
    expect(decimalsFor(0.5)).toBe(1);
    expect(decimalsFor(0.05)).toBe(2);
    expect(decimalsFor(10)).toBe(0);
  });
});

describe('formatValue', () => {
  it('passes strings through and formats numbers', () => {
    expect(formatValue('Equities')).toBe('Equities');
    expect(formatValue(0.25, 'pct')).toBe('25.00%');
    expect(formatValue(null)).toBe('–');
    expect(formatValue(false)).toBe('–');
  });
});
