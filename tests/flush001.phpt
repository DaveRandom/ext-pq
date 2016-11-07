--TEST--
flush
--SKIPIF--
<?php include "_skipif.inc"; ?>
--FILE--
<?php
echo "Test\n";

include "_setup.inc";

$length = 10000000;

$c = new pq\Connection(PQ_DSN);
$c->nonblocking = true;
var_dump($c->nonblocking);
$c->execAsync("SELECT '".str_repeat("a", 1e7)."'", function($r) use($length) {
	$r->fetchCol($s);
	var_dump(strlen($s) === $length);
});

$flushed = $c->flush();

var_dump('F', $flushed);

$lastFlushed = $flushed;
$lastPoll = -1;

do {
	while (!$flushed || $c->busy) {
		$r = $c->busy ? [$c->socket] : null;
		$w = !$flushed ?[$c->socket] : null; 
		
		if (stream_select($r, $w, $e, null)) {
			if ($r) {
				$poll = $c->poll();
				if ($lastPoll !== $poll) {
					var_dump('P', $poll);
					$lastPoll = $poll;
				}
			}
			if ($w) {
				$flushed = $c->flush();
				if ($lastFlushed !== $flushed) {
					var_dump('F', $flushed);
					$lastFlushed = $flushed;
				}
			}
		}
	}
} while ($c->getResult());
?>
===DONE===
--EXPECTF--
Test
bool(true)
string(1) "F"
bool(false)
string(1) "F"
bool(true)
string(1) "P"
int(3)
bool(true)
===DONE===
