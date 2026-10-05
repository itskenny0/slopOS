#!/usr/bin/env python3
"""v1.0 phase A: translate every Dutch user-facing string to English."""
import sys

def apply(path, pairs, allow_all=False):
    src = open(path, encoding='utf-8').read()
    for old, new in pairs:
        n = src.count(old)
        if n == 0:
            print(f"MISS   {path}: {old[:60]!r}")
            continue
        if n > 1 and not allow_all:
            print(f"AMBIG  {path} ({n}x): {old[:60]!r}")
        src = src.replace(old, new)
        print(f"ok     {path}: {old[:44]!r} -> {new[:44]!r}")
    open(path, 'w', encoding='utf-8').write(src)

shell = [
    ('n->link ? "verbinding" : "geen verbinding"',
     'n->link ? "link up" : "no link"'),
    ('puts("      geen adres -- `dhcp` vraagt de router om een\\n");',
     'puts("      no address -- `dhcp` asks the router for one\\n");'),
    ('"\\n  geen netwerkkaart\\n\\n"', '"\\n  no network card\\n\\n"'),
    ('puts("\\n  geen netwerkkaart, of geen adres. typ eerst `ip auto`\\n\\n");',
     'puts("\\n  no network card, or no address. run `ip auto` first\\n\\n");'),
    ('puts("\\n  geen netwerkkaart, of er kwam geen adres\\n\\n");',
     'puts("\\n  no network card, or no address came back\\n\\n");'),
    ('puts("\\n  de router om een adres vragen...\\n");',
     'puts("\\n  asking the router for an address...\\n");'),
    ('puts("  niemand antwoordde.\\n");',
     'puts("  nobody answered.\\n");'),
    ('kprintf("\\n  de kaart probeerde %u pakket%s, %u ging er echt weg, "\n                "en %u kwam terug.\\n",',
     'kprintf("\\n  the card tried %u packet%s, %u actually left, "\n                "and %u came back.\\n",'),
    ('puts("  er ging helemaal niks weg -- check de kabel, en het\\n"\n                 "  lampje van het stopcontact waar hij in zit.\\n");',
     'puts("  nothing left the card at all -- check the cable, and\\n"\n                 "  the link light on the socket it sits in.\\n");'),
    ('puts("  er ging wat weg en niks kwam terug -- de router antwoordt\\n"\n                 "  op een adres waar deze kaart niet naar luistert, of het\\n"\n                 "  antwoord komt niet aan. `net diag` print het adres.\\n");',
     'puts("  packets left but nothing came back -- the router replies\\n"\n                 "  to an address this card is not listening on, or the\\n"\n                 "  reply does not arrive. `net diag` prints the address.\\n");'),
    ('puts("  verkeer beide kanten op, maar geen adres -- deze router\\n"\n                 "  geeft geen adres aan een machine die hij niet kent.\\n"\n                 "\\n  geeft niks. typ:\\n\\n     ip auto\\n\\n"\n                 "  dan lees ik het netwerk zelf van de kabel af.\\n");',
     'puts("  traffic both ways, but no address -- this router does\\n"\n                 "  not hand addresses to machines it does not know.\\n"\n                 "\\n  no problem. type:\\n\\n     ip auto\\n\\n"\n                 "  and I will read the network off the cable myself.\\n");'),
    ('kprintf("  %u bytes  te kort voor een frame\\n", len);',
     'kprintf("  %u bytes  too short for a frame\\n", len);'),
    ('kprintf("\\n  ik luister naar %d pakket%s, maximaal dertig seconden\\n\\n",',
     'kprintf("\\n  listening for %d packet%s, thirty seconds at most\\n\\n",'),
    ('puts("  probeer maar:  web example.com\\n\\n");',
     'puts("  try it:  web example.com\\n\\n");'),
    ('puts("\\n  dat is geen adres\\n\\n");',
     'puts("\\n  that is not an address\\n\\n");'),
    ('puts("  probeer de gateway:  ping <gateway>\\n");',
     'puts("  try the gateway:  ping <gateway>\\n");'),
    ('puts("  om iets op te halen heb je ook het dns-adres nodig:\\n"\n         "     ip <adres> <netmask> <gateway> <dns>\\n\\n");',
     'puts("  to fetch anything you also need a dns address:\\n"\n         "     ip <address> <netmask> <gateway> <dns>\\n\\n");'),
    ('puts("\\n  ping <adres of naam> [hoeveel]\\n\\n");',
     'puts("\\n  ping <address or name> [count]\\n\\n");'),
    ('puts("\\n  geen antwoord van de naamserver\\n\\n");',
     'puts("\\n  no answer from the name server\\n\\n");'),
    ('kprintf("  antwoord in %d ms\\n", rtt);',
     'kprintf("  answer in %d ms\\n", rtt);'),
    ('puts("  geen antwoord\\n");',
     'puts("  no answer\\n");'),
    ('kprintf("\\n  %d van de %d antwoordde\\n\\n", answered, times);',
     'kprintf("\\n  %d of %d answered\\n\\n", answered, times);'),
]

net = [
    ('kprintf("\\n  niemand antwoordde. ik lees het netwerk zelf van de kabel\\n"\n            "  af -- aan het luisteren...\\n");',
     'kprintf("\\n  nobody answered. I will read the network off the cable\\n"\n            "  myself -- listening...\\n");'),
    ('kprintf("\\n  acht seconden en nog geen een pakket. de kabel is stil,\\n"\n                "  dus er valt niks af te kijken. check de kabel, of typ\\n"\n                "  het adres zelf:\\n"\n                "     ip <adres> 255.255.255.0 <gateway> 8.8.8.8\\n\\n");',
     'kprintf("\\n  eight seconds and not one packet. the cable is silent,\\n"\n                "  so there is nothing to copy from. check the cable, or\\n"\n                "  type the address yourself:\\n"\n                "     ip <address> 255.255.255.0 <gateway> 8.8.8.8\\n\\n");'),
    ('kprintf("  %s gehoord -- dit is dus %u.%u.%u.x\\n\\n",',
     'kprintf("  heard %s -- so this is %u.%u.%u.x\\n\\n",'),
    ('kprintf("  de uitgang is %s -- hij antwoordt op namen.\\n", b);',
     'kprintf("  the way out is %s -- it answers names.\\n", b);'),
    ('kprintf("  ik ben vanaf nu %s.\\n\\n", b);',
     'kprintf("  from now on I am %s.\\n\\n", b);'),
    ('kprintf("  niemand antwoordde op een naam, dus ik gok: gateway %s, en\\n"\n            "  ik vraag google wel naar namen. probeer `ping %s`\\n\\n",',
     'kprintf("  nobody answered a name, so I guess: gateway %s, and\\n"\n            "  I ask google for names. try `ping %s`\\n\\n",'),
]

draw = [
    ('api->puts("PICO TEKEN");', 'api->puts("PICO DRAW");'),
    ('"tekening.pic"', '"drawing.pic"'),
    ('say("saved as tekening.pic");', 'say("saved as drawing.pic");'),
    ('say("loaded tekening.pic");', 'say("loaded drawing.pic");'),
]

apply('kernel/shell.c', shell)
apply('net/net.c', net)
apply('apps/draw.c', draw)
print("done")
