import { describe, expect, it } from 'vitest'
import { episodeCode, formatClock, formatPairCode, formatRuntime, fraction, imageSrc, itemMeta } from '../src/lib/format.js'

describe('format', () => {
  it('formats clocks', () => {
    expect(formatClock(0)).toBe('0:00')
    expect(formatClock(42_999)).toBe('0:42')
    expect(formatClock(725_000)).toBe('12:05')
    expect(formatClock(3_723_000)).toBe('1:02:03')
    expect(formatClock(-5)).toBe('0:00')
    expect(formatClock(NaN)).toBe('0:00')
  })
  it('formats runtimes', () => {
    expect(formatRuntime(0)).toBe('')
    expect(formatRuntime(20_000)).toBe('1 min')
    expect(formatRuntime(48 * 60_000)).toBe('48 min')
    expect(formatRuntime(92 * 60_000)).toBe('1 h 32 min')
    expect(formatRuntime(120 * 60_000)).toBe('2 h')
  })
  it('labels episodes and items', () => {
    expect(episodeCode({ type: 'Episode', parentIndex: 1, index: 2 })).toBe('S1 · E2')
    expect(episodeCode({ type: 'Movie', parentIndex: 1, index: 2 })).toBe('')
    expect(itemMeta({ type: 'Movie', year: 2019, runtimeMs: 4_800_000 })).toBe('2019 · 1 h 20 min')
    expect(itemMeta({ type: 'Series', year: 2024 })).toBe('2024')
  })
  it('builds same-origin image URLs with snapped widths', () => {
    expect(imageSrc({ id: 'a'.repeat(32), type: 'Primary', tag: 'p1' }, 144, 3)).toBe(
      `/api/v1/images/${'a'.repeat(32)}/Primary?w=480&tag=p1`)
    expect(imageSrc({ id: 'b'.repeat(32), type: 'Thumb' }, 2000, 1)).toContain('w=1280')
    expect(imageSrc(null, 100)).toBe('')
    expect(imageSrc({ id: '../x', type: 'Primary' }, 100)).toContain('..%2Fx')
  })
  it('computes fractions safely', () => {
    expect(fraction(50, 100)).toBe(0.5)
    expect(fraction(150, 100)).toBe(1)
    expect(fraction(5, 0)).toBe(0)
  })
  it('normalizes pairing codes', () => {
    expect(formatPairCode('abcd')).toBe('ABCD')
    expect(formatPairCode('abcdefgh')).toBe('ABCD-EFGH')
    expect(formatPairCode(' ab-cd ef gh xyz')).toBe('ABCD-EFGH')
  })
})
