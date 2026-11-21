// @vitest-environment jsdom
import { cleanup, fireEvent, render, screen } from '@solidjs/testing-library';
import { afterEach, describe, expect, it } from 'vitest';
import { Select, Slider, Toggle, parseOptions } from './inputs';
import { captureSent, n } from './testing';

afterEach(cleanup);

describe('Slider', () => {
  const node = n('slider', {
    label: 'WWR correlation',
    min: -1,
    max: 1,
    step: 0.05,
    value: 0.25,
    format: 'num',
    'on-change': { $h: 'r/s#on-change' },
  });

  it('renders the label and the formatted value', () => {
    render(() => <Slider node={node} />);
    expect(screen.getByLabelText('WWR correlation')).toBeTruthy();
    expect(screen.getByTestId('slider-value').textContent).toBe('0.250');
  });

  it('commits a snapped numeric value on change', () => {
    const sent = captureSent();
    render(() => <Slider node={node} />);
    const input = screen.getByLabelText('WWR correlation') as HTMLInputElement;
    input.value = '0.62';
    fireEvent.change(input);
    expect(sent.at(-1)).toEqual({ type: 'event', h: 'r/s#on-change', data: { value: 0.6 } });
    expect(screen.getByTestId('slider-value').textContent).toBe('0.600');
  });

  it('sends live updates while dragging (first immediately)', () => {
    const sent = captureSent();
    render(() => <Slider node={node} />);
    const input = screen.getByLabelText('WWR correlation') as HTMLInputElement;
    input.value = '-0.5';
    fireEvent.input(input);
    expect(sent).toHaveLength(1);
    expect(sent[0].data).toEqual({ value: -0.5 });
  });

  it('live: #f sends only on commit', () => {
    const sent = captureSent();
    render(() => <Slider node={{ ...node, p: { ...node.p, live: false } }} />);
    const input = screen.getByLabelText('WWR correlation') as HTMLInputElement;
    input.value = '0.1';
    fireEvent.input(input);
    expect(sent).toHaveLength(0);
    fireEvent.change(input);
    expect(sent).toHaveLength(1);
  });

  it('without a handler nothing is sent', () => {
    const sent = captureSent();
    render(() => <Slider node={n('slider', { label: 'x', min: 0, max: 1, value: 0 })} />);
    const input = screen.getByLabelText('x') as HTMLInputElement;
    input.value = '1';
    fireEvent.change(input);
    expect(sent).toHaveLength(0);
  });
});

describe('Select', () => {
  it('parses string and pair options', () => {
    expect(parseOptions(['a', ['b', 'Bee']])).toEqual([
      { value: 'a', label: 'a' },
      { value: 'b', label: 'Bee' },
    ]);
    expect(parseOptions('nope')).toEqual([]);
  });

  it('sends the chosen value', () => {
    const sent = captureSent();
    render(() => (
      <Select node={n('select', {
        label: 'Shock driver',
        options: [['oil', 'Oil'], ['usd-10y', 'USD 10y']],
        value: 'oil',
        'on-change': { $h: 'r/sel#on-change' },
      })} />
    ));
    const select = screen.getByLabelText('Shock driver') as HTMLSelectElement;
    expect(select.options).toHaveLength(2);
    select.value = 'usd-10y';
    fireEvent.change(select);
    expect(sent.at(-1)).toEqual({ type: 'event', h: 'r/sel#on-change', data: { value: 'usd-10y' } });
  });
});

describe('Toggle', () => {
  it('reflects the value and sends the new state', () => {
    const sent = captureSent();
    render(() => (
      <Toggle node={n('toggle', { label: 'Collateralised', value: true, 'on-change': { $h: 'r/t#on-change' } })} />
    ));
    const box = screen.getByLabelText('Collateralised') as HTMLInputElement;
    expect(box.checked).toBe(true);
    fireEvent.click(box);
    expect(sent.at(-1)).toEqual({ type: 'event', h: 'r/t#on-change', data: { value: false } });
  });
});
