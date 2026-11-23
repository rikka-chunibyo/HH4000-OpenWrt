'use strict';
'require view';
'require form';
'require fs';
'require poll';
'require tools.widgets as widgets';
return view.extend({
 render: function() {
  var m = new form.Map('hh_sqm', _('Smart Queue Management'), _('FQ-CoDel manages queues to reduce latency under load. Hardware acceleration is paused while enabled, which increases CPU use. Set at least one nonzero rate before enabling.'));
  var s = m.section(form.NamedSection, 'main', 'sqm');
  var o = s.option(form.Flag, 'enabled', _('Enable SQM')); o.rmempty = false;
  o = s.option(widgets.NetworkSelect, 'network', _('Network')); o.default = 'wan'; o.rmempty = false; o.nocreate = true;
  o = s.option(widgets.DeviceSelect, 'device', _('Device override'), _('Leave empty to follow the selected network.')); o.rmempty = true; o.noaliases = true;
  o = s.option(form.Value, 'download', _('Download (kbit/s)'), _('Use 0 to leave this direction unshaped.')); o.datatype = 'and(uinteger,range(0,10000000))'; o.default = '0'; o.rmempty = false;
  o = s.option(form.Value, 'upload', _('Upload (kbit/s)'), _('Use 0 to leave this direction unshaped.')); o.datatype = 'and(uinteger,range(0,10000000))'; o.default = '0'; o.rmempty = false;
  var status = E('p', {}, _('Loading status...'));
  poll.add(function() { return fs.read('/var/run/hh-sqm/state.json').then(function(data) { var state = JSON.parse(data); status.textContent = state.status === 'running' ? _('Running on %s').format(state.device) : state.status === 'error' ? _('Error: %s').format(state.error) : _('Disabled'); }).catch(function() { status.textContent = _('Disabled'); }); }, 3);
  return m.render().then(function(node) { return E('div', {}, [node, status]); });
 }
});
