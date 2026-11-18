// Hash router: #/, #/library/<id>, #/item/<id>, #/search?q=..., #/now

const ID = /^[0-9a-f]{32}$/

export function parseRoute(hash) {
  const raw = String(hash || '').replace(/^#/, '') || '/'
  const [path, qs = ''] = raw.split('?')
  const query = Object.fromEntries(new URLSearchParams(qs))
  const parts = path.split('/').filter(Boolean)
  let route = { name: 'home', params: {} }
  if (parts[0] === 'library' && ID.test(parts[1] || '')) route = { name: 'library', params: { id: parts[1] } }
  else if (parts[0] === 'item' && ID.test(parts[1] || '')) route = { name: 'item', params: { id: parts[1] } }
  else if (parts[0] === 'search') route = { name: 'search', params: {} }
  else if (parts[0] === 'now') route = { name: 'now', params: {} }
  else if (parts.length) route = { name: 'notfound', params: {} }
  return { ...route, query, key: raw }
}

export function href(name, params = {}, query) {
  let p = '#/'
  if (name === 'library') p = `#/library/${params.id}`
  else if (name === 'item') p = `#/item/${params.id}`
  else if (name === 'search') p = '#/search'
  else if (name === 'now') p = '#/now'
  if (query) {
    const qs = new URLSearchParams(Object.entries(query).filter(([, v]) => v !== '' && v != null)).toString()
    if (qs) p += `?${qs}`
  }
  return p
}
