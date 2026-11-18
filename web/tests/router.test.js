import { describe, expect, it } from 'vitest'
import { href, parseRoute } from '../src/lib/router.js'

const ID = '0123456789abcdef0123456789abcdef'
describe('router', () => {
  it('parses routes', () => {
    expect(parseRoute('').name).toBe('home')
    expect(parseRoute('#/').name).toBe('home')
    expect(parseRoute(`#/item/${ID}`)).toMatchObject({ name: 'item', params: { id: ID } })
    expect(parseRoute(`#/library/${ID}`).params.id).toBe(ID)
    expect(parseRoute('#/search?q=harbor%20lights').query.q).toBe('harbor lights')
    expect(parseRoute('#/now').name).toBe('now')
  })
  it('rejects malformed ids and unknown paths', () => {
    expect(parseRoute('#/item/../../etc').name).toBe('notfound')
    expect(parseRoute('#/item/XYZ').name).toBe('notfound')
    expect(parseRoute('#/admin').name).toBe('notfound')
  })
  it('gives distinct keys per location', () => {
    expect(parseRoute(`#/item/${ID}`).key).not.toBe(parseRoute(`#/item/${ID.replace('0', '1')}`).key)
  })
  it('builds hrefs', () => {
    expect(href('item', { id: ID })).toBe(`#/item/${ID}`)
    expect(href('search', {}, { q: 'a b' })).toBe('#/search?q=a+b')
    expect(href('search', {}, { q: '' })).toBe('#/search')
    expect(href('home')).toBe('#/')
  })
})
