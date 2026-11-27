const fs = require('fs'), path = require('path'), assert = require('assert');
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(
    path.join(root,
              'src/openwrt-hh4000/files/www/luci-static/resources/view/status/include/40_dhcp.js'),
    'utf8');
String.prototype.format = function(...args) {
  let i = 0;
  return this.replace(/%[st]/g, () => String(args[i++]));
};
const tables = [];
function E(tag, attrs, children) {
  return {tag, attrs, children};
}
const L = {
  bind : (f, t, ...a) => f.bind(t, ...a),
  hasViewPermission : () => false,
  toArray : a => [a]
};
const view =
    new Function('baseclass', 'rpc', 'uci', 'network', 'validation', 'L', 'E', 'cbi_update_table',
                 '_', source)({extend : x => x}, {declare : () => () => {}}, {sections : () => []},
                              {}, {}, L, E, (t, rows) => tables.push(rows), s => s);
let checks = 0;
for (const hostname of ['<svg/onload=alert(1)>', '<img src=x onerror=alert(1)>', 'a\n# forged',
                        'normal.example']) {
  tables.length = 0;
  view.renderLeases([
    {
      dhcp_leases :
          [ {hostname, ipaddr : '192.168.1.22', macaddr : '02:00:00:00:00:01', expires : 60} ],
      dhcp6_leases :
          [ {hostname, ip6addrs : [ 'fd00::1', 'fd00::2' ], duid : '0011', expires : 60} ]
    },
    {getMACHints : () => []}
  ]);
  for (const rows of tables) {
    assert.equal(rows[0][0].tag, 'span');
    assert.deepEqual(rows[0][0].children, [ hostname ]);
    checks++;
  }
  assert.equal(tables[1][0][1].tag, 'div');
  assert.deepEqual(tables[1][0][1].children.map(x => x.children[0]), [ 'fd00::1', 'fd00::2' ]);
  checks++;
}
console.log(checks + ' lease-table text-node and IPv6 layout checks passed');
