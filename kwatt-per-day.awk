#!/usr/bin/gawk -f 
{
	if ($1 - lasttime < 3600) {
		day=strftime("%F", $1)
		a[day] += ($1 - lasttime) * $3
	}
	lasttime = $1
}
END {
	for (key in a) {
		kw = a[key] / (3600 * 1000)
		print key " " kw "kW £" kw * 0.124
		t += kw
		c += 1;
	}
	aver = t / c
	p = aver * 0.124
	print "average per day " aver "kW / £" p
}
