
import xml.etree.ElementTree as ET
import requests
import logging
import re
import sched
import time

fh = logging.FileHandler('bl_downloader.log')
cs = logging.StreamHandler()
formatter = logging.Formatter('[%(asctime)s] - %(levelname)s - %(message)s')
cs.setLevel(logging.DEBUG)
fh.setLevel(logging.DEBUG)
cs.setFormatter(formatter)
fh.setFormatter(formatter)

logger = logging.getLogger(__name__)
logger.addHandler(fh)
logger.addHandler(cs)
logger.setLevel(logging.DEBUG)

ip_regex = re.compile('\\b((2(5[0-5]|[0-4][0-9])|[01]?[0-9][0-9]?)\.){3}(2(5[0-5]|[0-4][0-9])|[01]?[0-9][0-9]?)((/(3[012]|[12]?[0-9]))?)\\b')

# Just very simple regex to eliminate commentaries
url_regex = re.compile('^[^#/].*\..+')

check_interval = 1 * 60 # Time period (secs) to check for blacklist changes

blacklists = []

# Sorting comparator, splits the IP in format "A.B.C.D(/X),Y,Z"
# into tuple of IP (A, B, C, D), which is comparable by python (numerically)
def split_ip(ip):
    # Extract only IP, without the prefix and indexes
    if '/' in ip:
        ip = ip.split('/')[0]
    else:
        ip = ip.split(',')[0]

    try:
        tuple_ip = tuple(int(part) for part in ip.split('.'))
    except ValueError as e:
        logger.warning('Couldnt sort this IP addr: {}'.format(ip))
        logger.warning(e)
        tuple_ip = (0, 0, 0, 0)

    """Split a IP address given as string into a 4-tuple of integers."""
    return tuple_ip


class Blacklist:
    def __init__(self, bl):
        self.entities = []
        self.last_download = None

        # Generate variables from the XML config file
        for element in bl:
            setattr(self, element.attrib['name'], element.text)

    def __str__(self):
        return str(self.__dict__)

    def download_and_update(self):
        updated = False

        try:
            req = requests.get(self.source, timeout=10)

            if req.status_code == 200:
                data = req.content

                new_entities = self.extract_entities(data)

                if new_entities != self.entities:
                    # Blacklist entities changed since last download
                    self.entities = new_entities
                    updated = True

                    logger.debug('Updated blacklist {}'.format(self.name))

            else:
                logger.warning('Couldnt fetch blacklist: {}\n'
                               'Status code: {}'.format(self.source, req.status_code))

        except requests.RequestException as e:
            logger.warning('Couldnt fetch blacklist: {}\n'
                           '{}'.format(self.source, e))

        return updated


class IPBlacklist(Blacklist):
    detector_file = 'bl_records_sorted_IP.txt'
    separator = ','
    comparator = split_ip

    def __init__(self, bl):
        super().__init__(bl)

    def extract_entities(self, data):
        extracted = []

        for line in data.decode('utf-8').splitlines():
            match = re.search(ip_regex, line)
            if match:
                extracted.append(match.group(0))

        return extracted


class URLBlacklist(Blacklist):
    detector_file = 'bl_records_sorted_URL.txt'
    separator = '\\'
    comparator = str

    def __init__(self, bl):
        super().__init__(bl)

    def extract_entities(self, data):
        extracted = []

        for line in data.decode('utf-8').splitlines():
            match = re.search(url_regex, line)
            if match:
                url = match.group(0)
                url = url.replace('https://', '', 1)
                url = url.replace('http://', '', 1)
                url = url.replace('www.', '', 1)
                url = url.lower()
                while url[-1] == '/':
                    url = url[:-1]
                # TODO: Maybe normalize also?
                extracted.append(url)

        return extracted


def create_detector_file(bl_type: Blacklist):
    all_entities = dict()

    # Enrich the entities with blacklist index (bitfield way), merge the indexes if the same entity
    # found on more blacklists
    for bl in blacklists:
        bl_idx = 2 ** (int(bl.id) - 1)
        for entity in bl.entities:
            if entity in all_entities.keys() and all_entities[entity] & bl_idx == 0:
                all_entities[entity] = all_entities[entity] + bl_idx
            else:
                all_entities[entity] = bl_idx

    # Create sorted list of entities and their cumulative indexes
    all_entities = sorted(['{entity}{sep}{idx}'.format(entity=entity, idx=idx, sep=bl_type.separator)
                           for entity, idx in all_entities.items()],
                           key=bl_type.comparator)

    with open(bl_type.detector_file, 'w') as f:
        f.write('\n'.join(all_entities))


def parse_config():
    tree = ET.parse("bld_userConfigFile.xml")
    bl_type_array = tree.getroot().getchildren()[0].getchildren()

    for bl_type in bl_type_array:
        type = bl_type.attrib['type']
        for bl in bl_type:
            if type == "IP":
                blacklists.append(IPBlacklist(bl))
            elif type == "URL":
                blacklists.append(URLBlacklist(bl))


def run(s):
    # schedule next check immediately
    s.enter(check_interval, 1, run, (s,))

    for bl_type in [IPBlacklist, URLBlacklist]:
        updated = False

        for bl in blacklists:
            if isinstance(bl, bl_type):
                # if bl.last_download and bl.last_download + 60 * bl.download_interval < time.time():
                updated += bl.download_and_update()

        if updated:
            create_detector_file(bl_type)
            logger.info('NEW {} Blacklist created'.format(bl_type.__name__))
        else:
            logger.info('Check for {} updates done, no changes'.format(bl_type.__name__))


if __name__ == '__main__':
    parse_config()

    s = sched.scheduler(time.time, time.sleep)

    s.enter(check_interval, 1, run, (s,))
    s.run()











