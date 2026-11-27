/* SPDX-License-Identifier: GPL-2.0-only */
'use strict';
'require view';
'require form';
'require fs';
'require ui';
'require poll';
'require uci';

function asciiTemplate(section, value) {
  return value.length <= 256 && /^[ -~]*$/.test(value) ||
         _('Use at most 256 printable ASCII characters.');
}

return view.extend({
  load : function() {
    return Promise.all([
      uci.load('hh_frontpanel'), fs.read('/usr/share/hh-frontpanel/example.py').catch(function() {
        return '';
      })
    ]);
  },
  render : function(data) {
    var m = new form.Map(
        'hh_frontpanel', _('Front Panel'),
        _('Choose Configuration or Python. Configuration uses your pages and button/LED rules. Python gives your program control and keeps its last frame until you change it or explicitly switch modes.'));
    var s = m.section(form.NamedSection, 'main', 'panel');
    s.addremove = false;
    s.tab('general', _('Mode and hardware'));
    s.tab('screen', _('Configuration: screen'));
    s.tab('buttons', _('Configuration: buttons'));
    s.tab('lights', _('Configuration: lights'));
    s.tab('python', _('Python program'));
    var o = s.taboption('general', form.ListValue, 'mode', _('Operating mode'));
    o.value('pages', _('Configuration'));
    o.value('python', _('Python'));
    o.default = 'pages';
    o.rmempty = false;
    o = s.taboption('general', form.Flag, 'enabled', _('Enable front panel'));
    o.default = '1';
    o.rmempty = false;
    o = s.taboption('general', form.Flag, 'screen', _('Enable screen'));
    o.default = '1';
    o.rmempty = false;
    o = s.taboption('general', form.Value, 'brightness', _('Brightness (%)'));
    o.default = '60';
    o.datatype = 'range(5,100)';
    o.rmempty = false;
    o = s.taboption(
        'general', form.Value, 'hold_ms', _('Hold threshold (milliseconds)'),
        _('Tap occurs on release. Hold fires once after this threshold. In Python mode, the program receives press, tap, hold and release events.'));
    o.default = '1000';
    o.datatype = 'range(300,5000)';
    o.rmempty = false;
    o = s.taboption(
        'general', form.ListValue, 'reset_action', _('RESET short press action'),
        _('A press under one second reboots by default, even in Python mode. Releasing between one second and the hold threshold cancels. The independent listener works with the panel disabled.'));
    o.value('reboot', _('Reboot router'));
    o.value('app', _('Panel program'));
    o.value('none', _('No system action'));
    [[ 'home', 'Home' ], [ 'previous', 'Previous page' ], [ 'next', 'Next page' ],
     [ 'menu', 'Menu' ], [ 'select', 'Select' ], [ 'blank', 'Toggle screen' ],
     [ 'wake', 'Wake screen' ]]
        .forEach(function(a) {
          o.value(a[0], _(a[1]));
        });
    o.default = 'reboot';
    o.rmempty = false;
    o = s.taboption(
        'general', form.Value, 'reset_hold_ms', _('RESET hold threshold (milliseconds)'),
        _('Default 15000 ms. The independent reset listener works even when the panel is disabled. Changing settings while held cancels arming until release.'));
    o.default = '15000';
    o.datatype = 'range(5000,60000)';
    o.rmempty = false;
    o = s.taboption(
        'general', form.ListValue, 'reset_hold', _('RESET long hold action'),
        _('Full factory reset erases installed packages, configuration and user data on the next boot. Firmware and hardware calibration are retained. This action is reserved even in Python mode. Choose Panel program to let Python own the hold instead.'));
    o.value('factory-reset', _('Full factory reset'));
    o.value('app', _('Panel program'));
    o.value('none', _('No system action'));
    [[ 'home', 'Home' ], [ 'previous', 'Previous page' ], [ 'next', 'Next page' ],
     [ 'menu', 'Menu' ], [ 'select', 'Select' ], [ 'blank', 'Toggle screen' ],
     [ 'wake', 'Wake screen' ]]
        .forEach(function(a) {
          o.value(a[0], _(a[1]));
        });
    uci.sections('hh_frontpanel', 'page').forEach(function(p) {
      o.value('page:' + p['.name'], _('Show page: %s').format(p.label || p['.name']));
    });
    o.default = 'factory-reset';
    o.rmempty = false;
    o = s.taboption(
        'general', form.Value, 'status_network', _('Status network'),
        _('Logical network used for {ip}, {status} and the Configuration status light. Status describes address readiness, not an internet connectivity test.'));
    o.default = 'lan';
    o.datatype = 'uciname';
    o.rmempty = false;

    o = s.taboption('screen', form.Value, 'title', _('Title template'));
    o.default = 'OpenWrt HH4000';
    o.validate = asciiTemplate;
    o.retain = true;
    o.depends('mode', 'pages');
    o = s.taboption(
        'screen', form.Value, 'idle_timeout', _('Screen timeout (seconds)'),
        _('Use 0 to keep the screen on. Python mode controls its own screen and does not use this timeout.'));
    o.default = '60';
    o.datatype = 'range(0,3600)';
    o.rmempty = false;
    o.retain = true;
    o.depends('mode', 'pages');
    o = s.taboption('screen', form.Value, 'rotate_seconds', _('Automatic page rotation (seconds)'));
    o.default = '0';
    o.datatype = 'range(0,3600)';
    o.description = _('0 disables rotation. The menu pauses rotation.');
    o.retain = true;
    o.depends('mode', 'pages');
    var configuredPages = uci.sections('hh_frontpanel', 'page');
    o = s.taboption('screen', form.Value, 'start_page', _('Home page ID'));
    configuredPages.forEach(function(p) {
      o.value(p['.name'], p.label || p['.name']);
    });
    o.default = 'home';
    o.datatype = 'uciname';
    o.retain = true;
    o.depends('mode', 'pages');
    ['message_line1', 'message_line2'].forEach(function(key, i) {
      o = s.taboption('screen', form.Value, key, _('Message line %d template').format(i + 1));
      o.validate = asciiTemplate;
      o.retain = true;
      o.depends('mode', 'pages');
    });

    var actions = [
      [ 'previous', _('Previous page / menu item') ], [ 'next', _('Next page / menu item') ],
      [ 'select', _('Open menu / select item') ], [ 'menu', _('Toggle menu') ],
      [ 'home', _('Home / back') ], [ 'blank', _('Toggle screen') ], [ 'wake', _('Wake screen') ],
      [ 'none', _('No action') ]
    ];
    configuredPages.forEach(function(p) {
      actions.push([ 'page:' + p['.name'], _('Show page: %s').format(p.label || p['.name']) ]);
    });
    [[ 'up', _('UP'), 'previous' ], [ 'ok', _('OK'), 'select' ], [ 'down', _('DOWN'), 'next' ],
     [ 'main', _('Main button'), 'home' ]]
        .forEach(function(item) {
          (item[0] === 'reset' ? [ [ 'action', _('tap'), item[2] ] ] : [
            [ 'action', _('tap'), item[2] ], [ 'hold', _('hold'), 'none' ]
          ]).forEach(function(kind) {
            o = s.taboption('buttons', form.ListValue, item[0] + '_' + kind[0],
                            item[1] + ' ? ' + kind[1]);
            actions.forEach(function(a) {
              o.value(a[0], a[1]);
            });
            o.default = kind[2];
            o.rmempty = false;
            o.retain = true;
            o.depends('mode', 'pages');
          });
        });
    o = s.taboption('buttons', form.DummyValue, '_button_mode', _('Python input'));
    o.default = _(
        'In Python mode these Configuration actions are ignored. Your program receives the button events and decides what they do. Holding the main button does not reboot the router. The separate RESET hold setting remains a system recovery action unless changed to Panel program or No system action.');

    [[
      'main_led', _('Main ring'),
      [ 'status', 'white', 'blue', 'red', 'off', 'blue:blink', 'red:fast', '{wan_up?white:blue}' ]
    ],
     [ 'up_led', _('UP light'), [ 'white', 'off', 'white:blink' ] ],
     [ 'ok_led', _('OK light'), [ 'white', 'amber', 'red', 'off', 'amber:blink', 'red:pulse' ] ], [
       'down_led', _('DOWN light'), [ 'white', 'off', 'white:blink' ]
     ]].forEach(function(item) {
      o = s.taboption('lights', form.Value, item[0], item[1]);
      item[2].forEach(function(value) {
        o.value(value);
      });
      o.default = item[2][0];
      o.rmempty = false;
      o.retain = true;
      o.depends('mode', 'pages');
      o.validate = function(section, value) {
        return value.length < 80 && /^[ -~]+$/.test(value) ||
               _('Use a color/pattern or a template, up to 79 characters.');
      };
      o.description = _(
          'Templates work here too. Patterns: :blink, :fast, :pulse or :awake. Unsupported colors resolve to off. UP/DOWN support white/off.');
    });
    o = s.taboption('lights', form.ListValue, 'navigation_leds', _('Navigation light policy'));
    o.value('active', _('While screen is awake'));
    o.value('always', _('Always'));
    o.value('off', _('Off'));
    o.default = 'active';
    o.rmempty = false;
    o.retain = true;
    o.depends('mode', 'pages');

    o = s.taboption(
        'python', form.TextValue, 'program', _('Python program'),
        _('Runs with router administrator privileges using the hhpanel API. Save & Apply starts or restarts it in Python mode. A stopped or failed program keeps its last frame; Configuration mode never takes over automatically.'));
    o.rows = 24;
    o.monospace = true;
    o.wrap = 'off';
    o.default = data[1];
    o.rmempty = false;
    o.retain = true;
    o.depends('mode', 'python');
    o.validate = function(section, value) {
      return new Blob([ value ]).size <= 32768 || _('Maximum program size is 32 KiB.');
    };
    o = s.taboption('python', form.DummyValue, '_program_actions', _('Saved program'));
    o.retain = true;
    o.depends('mode', 'python');
    o.renderWidget = function() {
      return E('div', {}, [
        E('button', {
          'type' : 'button',
          'class' : 'cbi-button cbi-button-action',
          'click' : ui.createHandlerFn(this,
                                       function() {
                                         return command('restart-program');
                                       })
        },
          _('Start / restart')),
        ' ',
        E('button', {
          'type' : 'button',
          'class' : 'cbi-button',
          'click' : ui.createHandlerFn(this,
                                       function() {
                                         return command('stop-program');
                                       })
        },
          _('Stop')),
        E('p', {},
          _('These controls use the saved program. Stopping it retains its last frame. A program that exits is not automatically restarted.'))
      ]);
    };
    o = s.taboption('python', form.DummyValue, '_example', _('Examples'));
    o.retain = true;
    o.depends('mode', 'python');
    o.renderWidget = function() {
      function load(path) {
        return fs.read(path)
            .then(function(text) {
              var option = m.lookupOption('program', 'main')[0],
                  widget = option && option.getUIElement('main');
              if (widget)
                widget.setValue(text);
            })
            .catch(function(err) {
              ui.addNotification(null, E('p', {}, err.message));
            });
      }
      return E('div', {}, [
        E('button', {
          'type' : 'button',
          'class' : 'cbi-button',
          'click' : ui.createHandlerFn(this,
                                       function() {
                                         return load('/usr/share/hh-frontpanel/example.py');
                                       })
        },
          _('Load interactive example')),
        ' ',
        E('button', {
          'type' : 'button',
          'class' : 'cbi-button',
          'click' : ui.createHandlerFn(this,
                                       function() {
                                         return load(
                                             '/usr/share/hh-frontpanel/variables-example.py');
                                       })
        },
          _('Load counter example')),
        E('pre', {},
          'from hhpanel import Panel\nwith Panel() as panel:\n    panel.write("{hostname}", "WAN {wan|waiting}")\n    panel.led("main", "blue")\n    panel.show()'),
        E('p', {},
          _('API: write, text, pixel, line, rectangle, led, screen, events, variables, publish and show. Standalone scripts: hh-panel-python /path/to/script.py.'))
      ]);
    };

    var pages = m.section(
        form.GridSection, 'page', _('Configuration mode pages'),
        _('Only Configuration mode uses these pages. Add, remove, reorder and edit up to 16 pages. Normal font: two lines of about 20 characters; small: four lines of about 42; large: one line of about 10. Longer expanded text is clipped.'));
    pages.anonymous = false;
    pages.addremove = true;
    pages.sortable = true;
    o = pages.option(form.Flag, 'enabled', _('Enabled'));
    o.default = '1';
    o.rmempty = false;
    o = pages.option(form.Value, 'label', _('Menu label template'));
    o.validate = asciiTemplate;
    o = pages.option(form.ListValue, 'font', _('Font'));
    o.value('normal', _('Normal'));
    o.value('small', _('Small'));
    o.value('large', _('Large'));
    o.default = 'normal';
    o.rmempty = false;
    o = pages.option(form.ListValue, 'align', _('Alignment'));
    o.value('left', _('Left'));
    o.value('center', _('Center'));
    o.value('right', _('Right'));
    o.default = 'center';
    o.modalonly = true;
    for (var i = 1; i <= 4; i++) {
      o = pages.option(form.Value, 'line' + i, _('Line %d template').format(i));
      o.validate = asciiTemplate;
      o.modalonly = true;
    }

    var canvas = E('canvas', {
      'width' : 168,
      'height' : 42,
      'style' :
          'width:504px;max-width:100%;height:auto;image-rendering:pixelated;background:#000;border:8px solid #222',
      'aria-label' : _('Live display preview')
    });
    var live = E('p', {'aria-live' : 'polite'}, _('Reading panel status?'));
    var app = E('p', {'aria-live' : 'polite', 'style' : 'white-space:pre-wrap'});
    var resetInfo = E('p', {'aria-live' : 'polite'});
    poll.add(function() {
      return fs.read('/var/run/hh-reset/state.json')
          .then(function(text) {
            var r = JSON.parse(text);
            resetInfo.textContent =
                _('RESET: %s; held %s s; threshold %s s.')
                    .format(r.factory_reset ? _('Full factory reset') : _('Panel control'),
                            r.held_seconds, r.hold_ms / 1000);
          })
          .catch(function() {
            resetInfo.textContent = _('Reset listener is starting or unavailable.');
          });
    }, 1);
    var values = E('pre', {'style' : 'white-space:pre-wrap'});
    function refresh() {
      return fs.read('/var/run/hh-frontpanel/state.json')
          .then(function(text) {
            var state = JSON.parse(text),
                mode = state.settings.mode === 'python' ? 'Python' : 'Configuration';
            live.textContent =
                _('Mode: %s ? Page: %s ? Screen: %s ? Button events: %s')
                    .format(mode, state.page, state.screen_on ? _('On') : _('Off'), state.events);
            if (state.frame_hex && state.frame_hex.length === 14112) {
              var context = canvas.getContext('2d'), pixels = context.createImageData(168, 42);
              for (var i = 0; i < 7056; i++) {
                var grey = parseInt(state.frame_hex.substr(i * 2, 2), 16) || 0;
                pixels.data[i * 4] = pixels.data[i * 4 + 1] = pixels.data[i * 4 + 2] = grey;
                pixels.data[i * 4 + 3] = 255;
              }
              context.putImageData(pixels, 0, 0);
            }
            values.textContent = Object.keys(state.variables || {})
                                     .sort()
                                     .map(function(k) {
                                       return '{' + k + '} = ' + state.variables[k];
                                     })
                                     .join('\n');
            return fs.read('/var/run/hh-frontpanel/app.json')
                .then(function(text) {
                  var a = JSON.parse(text);
                  app.textContent = mode !== 'Python' ? ''
                                    : a.error         ? _('Python error: %s').format(a.error)
                                    : a.running
                                        ? _('Python program is running.')
                                        : _('Python program stopped. Its last frame is retained.');
                })
                .catch(function() {
                  app.textContent =
                      mode === 'Python' ? _('Python program status is not available yet.') : '';
                });
          })
          .catch(function() {
            live.textContent = _('The front-panel service is stopped or starting.');
          });
    }
    function command(name) {
      return fs.exec('/usr/sbin/hh-frontpanel-control', [ name ])
          .then(function(r) {
            if (r.code)
              throw new Error(r.stderr || _('Panel command failed.'));
            return new Promise(function(resolve) {
              window.setTimeout(resolve, 100);
            });
          })
          .then(refresh)
          .catch(function(err) {
            ui.addNotification(null, E('p', {}, err.message));
          });
    }
    var controls = E('div', {'class' : 'cbi-section'},
                     [ E('h3', {}, _('Live panel')), canvas, live, app, resetInfo ]);
    [[ 'wake', _('Wake') ], [ 'up', _('UP tap') ], [ 'ok', _('OK tap') ], [ 'down', _('DOWN tap') ],
     [ 'main', _('Main tap') ], [ 'hold-main', _('Main hold') ], [ 'blank', _('Toggle screen') ]]
        .forEach(function(a) {
          controls.appendChild(E('button', {
            'type' : 'button',
            'class' : 'cbi-button cbi-button-action',
            'style' : 'margin:0 .4em .4em 0',
            'click' : ui.createHandlerFn(this,
                                         function() {
                                           return command(a[0]);
                                         })
          },
                                 a[1]));
        }, this);
    var help = E('details', {}, [
      E('summary', {}, _('Template variables and syntax')),
      E('p', {},
        _('Use variables in every screen text or menu label, and in Configuration LED fields. Examples: LAN {lan}, WAN {wan|waiting}, {wan_up?Online:Offline}, {status_color}:blink. Use {{ and }} for literal braces. Unknown variables remain visible. Templates do not execute code.')),
      E('p', {},
        _('Common variables: lan, wan, wan6, ip, status, hostname, uptime, load, memory, free_memory, time, date, ports, wifi, radio0, radio1, radio2, eth0?eth4 and eth0_speed?eth4_speed. Python can publish app.* variables for its own frames.')),
      values
    ]);
    poll.add(refresh, 1);
    return m.render().then(function(node) {
      return E('div', {}, [ controls, help, node ]);
    });
  }
});
