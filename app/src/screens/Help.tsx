// ---------------------------------------------------------------------------
//  Help.tsx - the whole user guide, inside the app.
//
//  It lives here rather than in a README because the moments you need it are
//  moments you are holding the tablet and standing next to the hardware: the
//  LED is doing something you don't recognise, or the device dropped off Wi-Fi,
//  or Alexa can't find it. Sending someone to a web page at that point is no
//  help at all - and if the device is offline, the page may be unreachable
//  anyway. Everything below works with no internet.
//
//  Sections are collapsed by default so the thing you need is one tap away
//  rather than a scroll through everything you don't.
// ---------------------------------------------------------------------------
import { useState } from 'react';
import type { JSX, ReactNode } from 'react';

interface Props {
  onClose: () => void;
}

function Section({
  title,
  subtitle,
  children,
  open,
  onToggle,
}: {
  title: string;
  subtitle: string;
  children: ReactNode;
  open: boolean;
  onToggle: () => void;
}): JSX.Element {
  return (
    <section className="card help-section">
      <button className="help-head" onClick={onToggle}>
        <span>
          <b>{title}</b>
          <span className="sub"> {subtitle}</span>
        </span>
        <span className="help-chev">{open ? '−' : '+'}</span>
      </button>
      {open && <div className="help-body">{children}</div>}
    </section>
  );
}

export function Help({ onClose }: Props): JSX.Element {
  const [open, setOpen] = useState<string | null>('led');
  const toggle = (id: string) => setOpen(open === id ? null : id);

  return (
    <div className="app help">
      <header className="top">
        <div>
          <h1>How to use this</h1>
          <p className="sub">Everything here works without internet</p>
        </div>
        <button className="sec" onClick={onClose}>
          Close
        </button>
      </header>

      {/* --- LED first: it is the fastest answer to "is it working?" ------- */}
      <Section
        title="Is it working?"
        subtitle="Read the light on the board"
        open={open === 'led'}
        onToggle={() => toggle('led')}
      >
        <p className="sub">
          The small blue LED on the ESP32 tells you everything, from across the room.
        </p>
        <table className="help-table">
          <tbody>
            <tr>
              <td>
                <b>Two flashes</b> every 3 seconds
              </td>
              <td className="ok">
                Fully online. You can control it from anywhere. This is the one you want.
              </td>
            </tr>
            <tr>
              <td>
                <b>One short flash</b> every 3 seconds
              </td>
              <td>On Wi-Fi, but not reachable from outside. Check the broker settings.</td>
            </tr>
            <tr>
              <td>
                <b>Slow blink</b>, once a second
              </td>
              <td>Trying to join Wi-Fi. Wrong password, or the network is out of range.</td>
            </tr>
            <tr>
              <td>
                <b>Fast blink</b>
              </td>
              <td>
                Setup mode. It has no Wi-Fi and has started its own. See <b>Change the Wi-Fi</b>.
              </td>
            </tr>
            <tr>
              <td>
                <b>Solid</b>
              </td>
              <td>Starting up. Give it 10 seconds.</td>
            </tr>
            <tr>
              <td>
                <b>Fast strobe</b>
              </td>
              <td>Installing a firmware update. Do not cut the power.</td>
            </tr>
            <tr>
              <td>
                <b>Triple blink</b>, repeating
              </td>
              <td className="bad">Fault - running low on memory. Power-cycle it.</td>
            </tr>
          </tbody>
        </table>
        <p className="sub hint">
          In this app: the badge at the top shows how you are connected, and each device says
          <b> online</b> or <b> offline</b>. If it says offline, the device lost its internet -
          the app is fine.
        </p>
      </Section>

      {/* --- the three ways to control it ---------------------------------- */}
      <Section
        title="Three ways to switch a socket"
        subtitle="Wall switch, app, Alexa"
        open={open === 'control'}
        onToggle={() => toggle('control')}
      >
        <table className="help-table">
          <tbody>
            <tr>
              <td>
                <b>Wall switch</b>
              </td>
              <td>
                Always works, instantly, even with no Wi-Fi and no internet. It never touches
                the network.
              </td>
            </tr>
            <tr>
              <td>
                <b>This app</b>
              </td>
              <td>Tap a tile. Works at home and, once set up, from anywhere.</td>
            </tr>
            <tr>
              <td>
                <b>Alexa</b>
              </td>
              <td>
                &quot;Alexa, turn on Socket 3.&quot; Works only when you are at home, on the
                same Wi-Fi.
              </td>
            </tr>
          </tbody>
        </table>
        <p className="sub hint">
          Whichever one you use, the other two update within a second. Each tile shows what
          caused the last change - &quot;wall switch&quot;, &quot;app&quot;, &quot;Alexa&quot;,
          &quot;schedule&quot;.
        </p>
      </Section>

      {/* --- changing Wi-Fi: the most common real task ---------------------- */}
      <Section
        title="Change the Wi-Fi"
        subtitle="New router, new password, moved house"
        open={open === 'wifi'}
        onToggle={() => toggle('wifi')}
      >
        <p>
          <b>If you can still reach the device</b> (you are on the same Wi-Fi):
        </p>
        <ol>
          <li>
            Open <code>http://smarthome-XXXX.local</code> in any browser
          </li>
          <li>
            Go to the <b>Wi-Fi</b> tab, tap <b>Scan</b>
          </li>
          <li>Tap your network, type the password, tap Save &amp; connect</li>
        </ol>

        <p>
          <b>If it has no network</b> - the LED is blinking fast:
        </p>
        <ol>
          <li>
            On your phone, join the Wi-Fi called <b>SmartHome-XXXX</b>
          </li>
          <li>
            The password is printed on the device, and is also in the serial log at first boot
          </li>
          <li>
            Open <code>http://192.168.4.1</code>
          </li>
          <li>Wi-Fi tab → Scan → pick your network → password → Save</li>
        </ol>

        <p className="sub hint">
          It remembers <b>up to 5 networks</b> and automatically joins whichever is strongest.
          Add your home Wi-Fi and your phone hotspot once, and it will switch between them on
          its own.
        </p>
        <p className="sub hint">
          It needs <b>2.4 GHz</b>. It cannot see 5 GHz networks at all - that is the radio in
          the chip, not a setting.
        </p>
      </Section>

      {/* --- remote --------------------------------------------------------- */}
      <Section
        title="Control it from the office"
        subtitle="What has to be true"
        open={open === 'remote'}
        onToggle={() => toggle('remote')}
      >
        <p>Two things, and nothing else:</p>
        <ol>
          <li>
            <b>The device has internet at home</b> - any Wi-Fi, any hotspot, any router
          </li>
          <li>
            <b>Your phone has internet</b> - mobile data, office Wi-Fi, anything
          </li>
        </ol>
        <p className="sub">
          They do not need to be the same network, the same city, or the same country. Neither
          one connects to the other: both connect out to a broker, and the broker passes
          messages between them.
        </p>
        <div className="warn">
          <b>The one thing that breaks it:</b> if the device&apos;s only internet is the phone
          in your pocket, then walking out of the house takes its internet with you. Leave a
          router, a spare phone, or a dongle at home.
        </div>
        <p className="sub hint">
          Set this up under <b>Anywhere</b> on the sign-in screen. The device needs the same
          broker details - port <b>8883</b> for the device, port <b>8884</b> for this app. Two
          different ports; mixing them up is the usual reason it does not work.
        </p>
      </Section>

      {/* --- alexa ---------------------------------------------------------- */}
      <Section
        title="Alexa"
        subtitle="Setup, and why discovery fails"
        open={open === 'alexa'}
        onToggle={() => toggle('alexa')}
      >
        <p>
          Say <b>&quot;Alexa, discover devices&quot;</b> and wait about 45 seconds. All your
          sockets appear. Nothing to buy, no account, no subscription - and it works with the
          internet down.
        </p>
        <p>
          <b>If she cannot find them</b>, it is almost always one of these three, and all three
          are router settings:
        </p>
        <ol>
          <li>
            The Echo and the device are on <b>different networks</b>. The device is 2.4 GHz
            only - put the Echo on 2.4 GHz too.
          </li>
          <li>
            <b>AP isolation</b> is switched on in your router. Turn it off.
          </li>
          <li>
            Your router is <b>blocking multicast</b>. Common on mesh systems.
          </li>
        </ol>
        <p className="sub hint">
          After renaming a socket, say &quot;Alexa, discover devices&quot; again - she
          remembers the old name otherwise. Give every socket a distinct, easy-to-say name.
        </p>
        <p className="sub hint">
          Alexa only works <b>at home</b>. Voice control from outside the house needs the cloud
          Alexa skill, which is not set up.
        </p>
      </Section>

      {/* --- restore -------------------------------------------------------- */}
      <Section
        title="After a power cut"
        subtitle="Choose what each socket does"
        open={open === 'restore'}
        onToggle={() => toggle('restore')}
      >
        <p>
          Open <b>Details</b> on a device. Each socket has three choices for what happens when
          power comes back:
        </p>
        <table className="help-table">
          <tbody>
            <tr>
              <td>
                <b>Stay off</b>
              </td>
              <td>
                Comes back off, whatever it was. Safest. Use this for anything that heats -
                geyser, iron, heater.
              </td>
            </tr>
            <tr>
              <td>
                <b>As before</b>
              </td>
              <td>
                Comes back exactly as you left it. Use this if one wall switch feeds the whole
                board, so flipping it back on restores everything.
              </td>
            </tr>
            <tr>
              <td>
                <b>Turn on</b>
              </td>
              <td>Always comes back on. For something that should never be off, like a fridge.</td>
            </tr>
          </tbody>
        </table>
        <p className="sub hint">
          Sockets come back about a second after power returns. Wi-Fi and Alexa take another
          ten seconds or so.
        </p>
      </Section>

      {/* --- wiring: safety-critical, kept blunt ---------------------------- */}
      <Section
        title="Before you wire it to mains"
        subtitle="Three rules that are not optional"
        open={open === 'wiring'}
        onToggle={() => toggle('wiring')}
      >
        <div className="warn">
          Mains wiring can kill you and can destroy the board. If you are not confident, have
          an electrician do it.
        </div>
        <ol>
          <li>
            <b>Fit a 10 kΩ pull-up resistor on all eight relay IN lines</b> (to 3.3 V). Without
            them, every socket switches on for a third of a second on every power-up. Software
            cannot fix this - the code is not running yet.
          </li>
          <li>
            <b>Remove the JD-VCC jumper</b> and give the relay coils their own 5 V / 2 A
            supply. Eight coils draw more than a USB port can supply. The symptom is random
            restarts that look exactly like a software crash.
          </li>
          <li>
            <b>Never connect a mains switch to the board.</b> Switch inputs must be dry
            contacts - just a switch between the pin and ground, carrying 3.3 V and nothing
            else.
          </li>
        </ol>
        <p className="sub hint">
          Also: use the <b>NO</b> contact so an unpowered board leaves sockets off, keep each
          channel under about 5 A, and never run a 16 A socket (AC, geyser, pump) through these
          relays - that needs a contactor.
        </p>
      </Section>

      {/* --- troubleshooting ------------------------------------------------ */}
      <Section
        title="When something is wrong"
        subtitle="Symptom to cause"
        open={open === 'trouble'}
        onToggle={() => toggle('trouble')}
      >
        <table className="help-table">
          <tbody>
            <tr>
              <td>App says the device is offline</td>
              <td>The device lost internet. Check its LED - fast blink means no Wi-Fi.</td>
            </tr>
            <tr>
              <td>Tile flips back after I tap it</td>
              <td>The command did not reach the device. It will be resent when it reconnects.</td>
            </tr>
            <tr>
              <td>Everything is slow or drops</td>
              <td>
                Weak Wi-Fi. Check the signal in <b>Details</b> - below −78 dBm is trouble.
              </td>
            </tr>
            <tr>
              <td>Device restarts on its own</td>
              <td className="bad">
                Almost always the 5 V supply, not the software. See the wiring section.
              </td>
            </tr>
            <tr>
              <td>All relays click on power-up</td>
              <td className="bad">The pull-up resistors are missing.</td>
            </tr>
            <tr>
              <td>Alexa cannot find the sockets</td>
              <td>Wrong Wi-Fi band, AP isolation, or multicast blocked. See the Alexa section.</td>
            </tr>
            <tr>
              <td>Schedules are not running</td>
              <td>
                The device has no clock until it reaches the internet once. It refuses to guess
                the time rather than fire at the wrong moment.
              </td>
            </tr>
            <tr>
              <td>Forgotten everything, want to start over</td>
              <td>
                Hold the <b>BOOT</b> button on the board for 10 seconds. This erases Wi-Fi,
                names and settings.
              </td>
            </tr>
          </tbody>
        </table>
      </Section>

      {/* --- limits: honest, so nothing is a surprise ----------------------- */}
      <Section
        title="What it does not do"
        subtitle="Worth knowing up front"
        open={open === 'limits'}
        onToggle={() => toggle('limits')}
      >
        <ul>
          <li>
            <b>No clock of its own.</b> After a power cut with no internet it does not know the
            time, so schedules wait rather than fire at the wrong moment.
          </li>
          <li>
            <b>Alexa is home-only.</b> Voice from outside the house is not set up.
          </li>
          <li>
            <b>Anyone on your Wi-Fi can control it.</b> The local connection is not encrypted.
          </li>
          <li>
            <b>Anyone holding the board can read its passwords</b> over a USB cable. Physical
            access is full access.
          </li>
          <li>
            <b>On/off only.</b> No dimming - these are relays, not dimmers.
          </li>
        </ul>
      </Section>
    </div>
  );
}
